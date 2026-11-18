//
// Copyright (c) 2026 Virtual Serial Studio contributors
//
// SPDX-License-Identifier: GPL-3.0-or-later
//
// Adding a monitor to a real device and to a port made for it.
//

#include "support/scripted_transport.hpp"
#include "support/test_support.hpp"

#include <vss/studio/studio.hpp>

#include <memory>
#include <string>

using namespace vss;
using namespace vss::testkit;

namespace
{

/// A studio whose addresses open a scripted transport, as a real device that
/// is plugged in does.
studio_options device_options()
{
    studio_options made;
    made.make_transport = [](const transport::executor_type& executor,
                             const std::string&, const serial_settings*,
                             error_code&) -> std::shared_ptr<transport>
    { return std::make_shared<vss_test_support::scripted_transport>(executor); };
    return made;
}

} // namespace

TEST_CASE("studio: a monitor watches a device and a new virtual port")
{
    studio app(device_options());
    const auto device = app.attach_serial("port 1", "COM5");
    REQUIRE(device.has_value());
    const auto copy = app.create_virtual_port(
        "port 1-monitor", virtual_port_kind::loopback, {});
    REQUIRE(copy.has_value());

    const auto linked = app.monitor("port 1", "port 1-monitor");
    REQUIRE(linked.has_value());
    REQUIRE(app.engine().links().size() == 1);
}
