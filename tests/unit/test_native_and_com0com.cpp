//
// Copyright (c) 2026 Virtual Serial Studio contributors
//
// SPDX-License-Identifier: GPL-3.0-or-later
//

#include <vss/ports/com0com.hpp>
#include <vss/ports/native_lines.hpp>
#include <vss/ports/serial_enumeration.hpp>
#include <vss/ports/serial_transport.hpp>
#include <vss/ports/virtual_port.hpp>

#include "test_framework.hpp"

#include <boost/asio/io_context.hpp>

using namespace vss;

TEST_CASE("enumeration: the COM name database bitmap gives the numbers kept")
{
    // Bit 0 of byte 0 is COM1; the bits are the Windows database's.
    REQUIRE(com_numbers_in_bitmap({}).empty());
    REQUIRE(com_numbers_in_bitmap(std::vector<unsigned char>(32, 0)).empty());
    std::vector<unsigned char> bitmap(32, 0);
    bitmap[0] = 0xFC;  // COM3 to COM8
    bitmap[1] = 0x01;  // COM9
    bitmap[31] = 0x40; // COM255
    const std::vector<unsigned> expected{3, 4, 5, 6, 7, 8, 9, 255};
    REQUIRE(com_numbers_in_bitmap(bitmap) == expected);

    // Bits past COM255 and bytes past the 32 that cover it are not numbers.
    bitmap[31] = 0xFF;
    bitmap.push_back(0xFF);
    const auto numbers = com_numbers_in_bitmap(bitmap);
    REQUIRE_EQ(numbers.back(), 255u);
    // COM3 to COM9, then COM249 to COM255: the eighth bit of the last byte
    // would be COM256.
    REQUIRE_EQ(numbers.size(), std::size_t{7 + 7});
}

TEST_CASE("enumeration: what holds each COM number")
{
    const auto entry = [](unsigned number, const char* id, bool present)
    {
        com_port_entry e;
        e.number = number;
        e.instance_id = id;
        e.description = "port";
        e.present = present;
        return e;
    };
    const auto find = [](const std::vector<com_number_holder>& rows,
                         unsigned number)
    {
        for (const auto& row : rows)
        {
            if (row.number == number)
                return row;
        }
        return com_number_holder{};
    };

    const auto rows = classify_com_numbers(
        {1, 2, 3, 4, 5, 10},
        {3, 7},
        {entry(1, "SWD\\VSSCOMUM\\0003", false),   // Studio, removed
         entry(2, "SWD\\VSSCOMUM\\0000", true),    // Studio, running
         entry(4, "USB\\VID_303A&PID_1001\\1", false), // unplugged adapter
         entry(5, "COM0COM\\PORT\\CNCB1", true)});
    REQUIRE_EQ(rows.size(), std::size_t{7});

    REQUIRE(find(rows, 1).kind == com_holder_kind::studio_port_gone);
    REQUIRE(find(rows, 1).reusable_by_studio());
    REQUIRE(find(rows, 2).kind == com_holder_kind::present_device);
    REQUIRE(!find(rows, 2).reusable_by_studio());
    // Marked and resolving, no device listed.
    REQUIRE(find(rows, 3).kind == com_holder_kind::name_only);
    REQUIRE(!find(rows, 3).reusable_by_studio());
    REQUIRE(find(rows, 4).kind == com_holder_kind::other_port_gone);
    REQUIRE(!find(rows, 4).reusable_by_studio());
    REQUIRE(find(rows, 5).kind == com_holder_kind::present_device);
    // Resolves only.
    REQUIRE(find(rows, 7).kind == com_holder_kind::name_only);
    // Only the database marks it.
    REQUIRE(find(rows, 10).kind == com_holder_kind::database_only);
    REQUIRE(find(rows, 10).reusable_by_studio());
    // Sorted.
    REQUIRE_EQ(rows.front().number, 1u);
    REQUIRE_EQ(rows.back().number, 10u);

    // A present device beats a gone one with the same number.
    const auto both = classify_com_numbers(
        {6}, {}, {entry(6, "SWD\\VSSCOMUM\\0001", false),
                  entry(6, "USB\\X\\1", true)});
    REQUIRE_EQ(both.size(), std::size_t{1});
    REQUIRE(both[0].kind == com_holder_kind::present_device);
}

TEST_CASE("enumeration: Studio port instance ids")
{
    REQUIRE(is_studio_port_instance_id("SWD\\VSSCOMUM\\0003"));
    REQUIRE(is_studio_port_instance_id("VSSCOM\\PORT\\0001"));
    REQUIRE(!is_studio_port_instance_id("USB\\VID_303A&PID_1001\\1"));
    REQUIRE(!is_studio_port_instance_id(""));
}

TEST_CASE("enumeration: the kept COM numbers are a sorted set or unknown")
{
    // Windows answers; every other system says it cannot be asked.
    const auto kept = reserved_com_numbers();
    if (kept)
    {
        for (std::size_t index = 1; index < kept->size(); ++index)
            REQUIRE((*kept)[index - 1] < (*kept)[index]);
        for (const unsigned number : *kept)
            REQUIRE(number >= 1 && number <= 255);
    }
}

TEST_CASE("enumeration: COM names are parsed from descriptions")
{
    REQUIRE_EQ(parse_com_port_name("USB Serial Device (COM13)"),
               std::string("COM13"));
    REQUIRE_EQ(parse_com_port_name("com0com - serial port emulator (COM3)"),
               std::string("COM3"));
    REQUIRE_EQ(parse_com_port_name("(com5)"), std::string("com5"));
    REQUIRE_EQ(parse_com_port_name("no port here"), std::string());
    REQUIRE_EQ(parse_com_port_name("(COM)"), std::string());
    REQUIRE_EQ(parse_com_port_name("(COMX)"), std::string());
    REQUIRE_EQ(parse_com_port_name("(ttyUSB0)"), std::string());
    REQUIRE_EQ(parse_com_port_name("(COM12"), std::string());
    REQUIRE_EQ(parse_com_port_name("COM3 without brackets"), std::string());
}

TEST_CASE("enumeration: com0com instance ids are parsed")
{
    const auto first = parse_com0com_instance_id("COM0COM\\PORT\\CNCA1");
    REQUIRE(first);
    REQUIRE_EQ(first->pair, 1);
    REQUIRE_EQ(first->side, std::string("A"));

    const auto second = parse_com0com_instance_id("COM0COM\\PORT\\CNCB12");
    REQUIRE(second);
    REQUIRE_EQ(second->pair, 12);
    REQUIRE_EQ(second->side, std::string("B"));

    const auto lower = parse_com0com_instance_id("com0com\\port\\cnca2");
    REQUIRE(lower);
    REQUIRE_EQ(lower->pair, 2);
    REQUIRE_EQ(lower->side, std::string("A"));

    // The numeric suffix ends at the first non digit.
    const auto trailing = parse_com0com_instance_id("CNCA1X");
    REQUIRE(trailing);
    REQUIRE_EQ(trailing->pair, 1);

    REQUIRE(!parse_com0com_instance_id(""));
    REQUIRE(!parse_com0com_instance_id("USB\\VID_1234"));
    REQUIRE(!parse_com0com_instance_id("CNC"));
    REQUIRE(!parse_com0com_instance_id("CNCZ1"));
    REQUIRE(!parse_com0com_instance_id("CNCA"));
    REQUIRE(!parse_com0com_instance_id("CNCAx"));
}

TEST_CASE("native lines: invalid handles report errors")
{
    REQUIRE(native::supported());

    REQUIRE(!native::read_modem_status(0));
    REQUIRE(!native::write_line(0, control_line::dtr, true));
    REQUIRE(!native::set_break(0, true));

    // A plausible but invalid handle exercises the platform call itself.
    REQUIRE(!native::read_modem_status(static_cast<std::intptr_t>(1234)));
    REQUIRE(!native::write_line(static_cast<std::intptr_t>(1234),
                                control_line::dtr, true));
    REQUIRE(!native::set_break(static_cast<std::intptr_t>(1234), true));
}

TEST_CASE("com0com: installation detection is consistent")
{
    const auto installation = detect_com0com();
    REQUIRE_EQ(installation.found, !installation.setupc_path.empty());
    if (installation.found)
        REQUIRE(!installation.install_dir.empty());

    for (const auto& pair : installation.pairs)
        REQUIRE(pair.index >= 0);

    const auto regrouped = group_com0com_pairs(enumerate_serial_devices());
    REQUIRE_EQ(regrouped.size(), installation.pairs.size());
}

TEST_CASE("com0com: nothing is created without an installation")
{
    com0com_installation missing;
    REQUIRE_EQ(create_com0com_pair(missing, "COM10", "COM11", true),
               make_error_code(errc::unsupported));
    REQUIRE_EQ(remove_com0com_pair(missing, 0, true),
               make_error_code(errc::unsupported));
}

TEST_CASE("serial transport: mock handles have no native handle")
{
    boost::asio::io_context io;
    boost::system::error_code error;
    auto transport = serial_transport::make_mock(
        io.get_executor(), serial_settings{}, "handle", error);
    REQUIRE(!error);
    REQUIRE_EQ(transport->native_handle(), std::intptr_t{0});
    REQUIRE(!transport->supports_control_lines());
}
