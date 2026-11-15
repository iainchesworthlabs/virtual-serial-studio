//
// Copyright (c) 2026 Virtual Serial Studio contributors
//
// SPDX-License-Identifier: GPL-3.0-or-later
//

#include <vss/ports/com0com.hpp>
#include <vss/ports/serial_enumeration.hpp>
#include <vss/ports/vcom_driver.hpp>
#include <vss/ports/virtual_port.hpp>

#include "test_framework.hpp"

#include <boost/asio/io_context.hpp>

#include <algorithm>
#include <string>

using namespace vss;

TEST_CASE("virtual ports: the Windows driver hooks stay honest")
{
    // Supports must mirror the driver probe, so a missing driver is reported
    // rather than discovered halfway through creating a port.
    REQUIRE_EQ(supports(virtual_port_kind::windows_driver), driver_available());
    REQUIRE_EQ(std::string(to_string(virtual_port_kind::windows_driver)),
               std::string("windows-driver"));

    boost::asio::io_context io;
    if (!driver_available())
    {
        REQUIRE(driver_version().empty());
        REQUIRE_EQ(create_virtual_port(io.get_executor(),
                                       virtual_port_kind::windows_driver, "x"),
                   make_error_code(errc::unsupported));

        const auto kinds = available_kinds();
        REQUIRE(std::find(kinds.begin(), kinds.end(),
                          virtual_port_kind::windows_driver) == kinds.end());
    }
}

TEST_CASE("virtual ports: platform support matrix")
{
    REQUIRE(supports(virtual_port_kind::loopback));
    REQUIRE(!supports(virtual_port_kind::named_pipe));

    const auto kinds = available_kinds();
    REQUIRE(!kinds.empty());
    REQUIRE(std::find(kinds.begin(), kinds.end(),
                      virtual_port_kind::loopback) != kinds.end());

    REQUIRE_EQ(std::string(to_string(virtual_port_kind::pty)),
               std::string("pty"));
    REQUIRE_EQ(std::string(to_string(virtual_port_kind::named_pipe)),
               std::string("named-pipe"));
    REQUIRE_EQ(std::string(to_string(virtual_port_kind::loopback)),
               std::string("loopback"));
}

TEST_CASE("virtual ports: loopback creation")
{
    boost::asio::io_context io;
    const auto created = create_virtual_port(
        io.get_executor(), virtual_port_kind::loopback, "unit");
    REQUIRE(created);
    REQUIRE(created->port != nullptr);
    REQUIRE_EQ(created->kind, virtual_port_kind::loopback);
    REQUIRE_EQ(created->path, std::string("loopback:unit"));
    REQUIRE(created->port->is_open());
    REQUIRE(created->port->supports_control_lines());
}

TEST_CASE("virtual ports: named pipes report unsupported")
{
    boost::asio::io_context io;
    REQUIRE_EQ(create_virtual_port(io.get_executor(),
                                   virtual_port_kind::named_pipe, "x"),
               make_error_code(errc::unsupported));
}

TEST_CASE("virtual ports: best kind is always available")
{
    // Where the driver is installed the best kind is a real COM port, and a
    // unit test does not make one on the machine it runs on.  The guest run
    // makes driver ports (tests/unit/platform/windows, VSS_TEST_DRIVER=1).
    if (driver_available())
        VSS_SKIP("the Windows driver is installed; it would make a real port");

    boost::asio::io_context io;
    const auto created = create_best_virtual_port(io.get_executor(), "best");
    REQUIRE(created);
    REQUIRE(created->port->is_open());
    REQUIRE(!created->path.empty());
}

TEST_CASE("enumeration: normalise sorts and removes duplicates")
{
    std::vector<serial_device_info> devices(5);
    devices[0].path = "COM5";
    devices[1].path = "COM3";
    devices[2].path = "COM3";
    devices[3].path = "";
    devices[4].path = "COM1";

    const auto normalised = normalise(devices);
    REQUIRE_EQ(normalised.size(), std::size_t{3});
    REQUIRE_EQ(normalised[0].path, std::string("COM1"));
    REQUIRE_EQ(normalised[1].path, std::string("COM3"));
    REQUIRE_EQ(normalised[2].path, std::string("COM5"));
}

TEST_CASE("enumeration: com0com ends are grouped into pairs")
{
    std::vector<serial_device_info> devices(5);

    devices[0].path = "COM3";
    devices[0].com0com = true;
    devices[0].com0com_pair = 1;
    devices[0].com0com_side = "A";

    devices[1].path = "COM5";
    devices[1].com0com = true;
    devices[1].com0com_pair = 1;
    devices[1].com0com_side = "B";

    devices[2].path = "COM7";
    devices[2].com0com = true;
    devices[2].com0com_pair = 2;
    devices[2].com0com_side = "A";

    devices[3].path = "COM9";
    devices[3].com0com = false;

    devices[4].path = "COM11";
    devices[4].com0com = true;
    devices[4].com0com_pair = -1;

    const auto pairs = group_com0com_pairs(devices);
    REQUIRE_EQ(pairs.size(), std::size_t{1});
    REQUIRE_EQ(pairs.front().index, 1);
    REQUIRE_EQ(pairs.front().a, std::string("COM3"));
    REQUIRE_EQ(pairs.front().b, std::string("COM5"));
}

TEST_CASE("enumeration: the machine can be enumerated")
{
    const auto devices = enumerate_serial_devices();
    for (const auto& device : devices)
        REQUIRE(!device.path.empty());

    // Enumerating twice must be stable.
    REQUIRE_EQ(enumerate_serial_devices().size(), devices.size());

    const auto names = enumerate_serial_port_names();
    REQUIRE_EQ(names.size(), devices.size());
}

TEST_CASE("virtual ports: a COM number outside 1 to 255 is refused wherever it is asked")
{
    // The same answer on every system and with or without the driver, so that a
    // mistake in a saved topology is caught at once.
    boost::asio::io_context io;
    for (const unsigned number : {256u, 1000u, 4294967295u})
    {
        REQUIRE_EQ(create_driver_port(io.get_executor(), "x", number),
                   make_error_code(errc::invalid_argument));
        REQUIRE_EQ(create_virtual_port(io.get_executor(),
                                       virtual_port_kind::windows_driver, "x",
                                       number),
                   make_error_code(errc::invalid_argument));
    }
    REQUIRE_EQ(static_cast<int>(min_driver_com_number), 1);
    REQUIRE_EQ(static_cast<int>(max_driver_com_number), 255);

    // A kind of port that has no COM numbers takes the number and ignores it.
    const auto loop = create_virtual_port(io.get_executor(),
                                          virtual_port_kind::loopback, "n", 7);
    REQUIRE(loop.has_value());
}

TEST_CASE("virtual ports: only a driver port can change its COM number")
{
    boost::asio::io_context io;
    auto loop = create_virtual_port(io.get_executor(),
                                    virtual_port_kind::loopback, "plain");
    REQUIRE(loop.has_value());

    // A number out of range is wrong whatever the port is.
    REQUIRE_EQ(set_driver_com_number(*loop->port, 0),
               make_error_code(errc::invalid_argument));
    REQUIRE_EQ(set_driver_com_number(*loop->port, 256),
               make_error_code(errc::invalid_argument));
    // A transport that is not a driver port has no number to change: the driver
    // exists only on Windows, where this is a wrong argument, and nowhere else,
    // where it is not available at all.
    const auto changed = set_driver_com_number(*loop->port, 7);
    REQUIRE(!changed.has_value());
    REQUIRE(changed.error() == make_error_code(errc::unsupported)
            || changed.error() == make_error_code(errc::invalid_argument));
}

TEST_CASE("virtual ports: nothing is known about the program on a port that is not ours")
{
    REQUIRE(!driver_peer("COM1").has_value());
    REQUIRE(!driver_peer("").has_value());
    REQUIRE(!driver_peer("/dev/pts/3").has_value());
}
