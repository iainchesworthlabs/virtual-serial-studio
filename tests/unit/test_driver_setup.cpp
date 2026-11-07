//
// Copyright (c) 2026 Virtual Serial Studio contributors
//
// SPDX-License-Identifier: GPL-3.0-or-later
//
// Which piece of the driver set-up is missing, from what the host says about
// each piece. Observations are injected, so every outcome is checked on every
// operating system without a driver.
//

#include "test_framework.hpp"

#include <vss/ports/driver_setup.hpp>

#include <string>

using namespace vss;

namespace
{

driver_setup_observation seen(driver_fact package, helper_service_state service,
                              helper_probe pipe, unsigned long code = 0)
{
    driver_setup_observation out;
    out.driver_installed = package;
    out.service = service;
    out.pipe = pipe;
    out.code = code;
    return out;
}

} // namespace

TEST_CASE("driver setup: everything in place is ready")
{
    REQUIRE(classify_driver_setup(seen(driver_fact::yes,
                                       helper_service_state::running,
                                       helper_probe::ok)) ==
            driver_setup_state::ready);
    // A driver store that could not be read does not hold a working helper up.
    REQUIRE(classify_driver_setup(seen(driver_fact::unknown,
                                       helper_service_state::running,
                                       helper_probe::ok)) ==
            driver_setup_state::ready);
}

TEST_CASE("driver setup: the kernel-mode control device needs no helper")
{
    driver_setup_observation kernel;
    kernel.control_device = true;
    kernel.driver_installed = driver_fact::yes;
    kernel.service = helper_service_state::not_installed;
    kernel.pipe = helper_probe::no_pipe;
    REQUIRE(classify_driver_setup(kernel) == driver_setup_state::ready);
}

TEST_CASE("driver setup: a PC with nothing installed is missing the driver first")
{
    REQUIRE(classify_driver_setup(seen(driver_fact::no,
                                       helper_service_state::not_installed,
                                       helper_probe::no_pipe)) ==
            driver_setup_state::driver_missing);
    // The helper is there and the driver is not.
    REQUIRE(classify_driver_setup(seen(driver_fact::no,
                                       helper_service_state::running,
                                       helper_probe::ok)) ==
            driver_setup_state::driver_missing);
}

TEST_CASE("driver setup: the helper service is missing or stopped")
{
    REQUIRE(classify_driver_setup(seen(driver_fact::yes,
                                       helper_service_state::not_installed,
                                       helper_probe::no_pipe, 2)) ==
            driver_setup_state::helper_missing);
    REQUIRE(classify_driver_setup(seen(driver_fact::yes,
                                       helper_service_state::stopped,
                                       helper_probe::no_pipe, 2)) ==
            driver_setup_state::helper_stopped);
    // Running with no pipe is not answering: the same fix as stopped.
    REQUIRE(classify_driver_setup(seen(driver_fact::yes,
                                       helper_service_state::running,
                                       helper_probe::no_pipe)) ==
            driver_setup_state::helper_stopped);
    // The service manager could not be asked, and there is no pipe.
    REQUIRE(classify_driver_setup(seen(driver_fact::yes,
                                       helper_service_state::unknown,
                                       helper_probe::no_pipe)) ==
            driver_setup_state::helper_stopped);
}

TEST_CASE("driver setup: an older helper is incompatible, not a permission problem")
{
    REQUIRE(classify_driver_setup(seen(driver_fact::yes,
                                       helper_service_state::running,
                                       helper_probe::incompatible, 50)) ==
            driver_setup_state::helper_incompatible);
}

TEST_CASE("driver setup: a refusal by the helper is the user's access")
{
    REQUIRE(classify_driver_setup(seen(driver_fact::yes,
                                       helper_service_state::running,
                                       helper_probe::access_denied, 5)) ==
            driver_setup_state::helper_denied);
}

TEST_CASE("driver setup: an exchange that failed is not guessed at")
{
    REQUIRE(classify_driver_setup(seen(driver_fact::yes,
                                       helper_service_state::running,
                                       helper_probe::failed, 109)) ==
            driver_setup_state::unknown);
    REQUIRE(classify_driver_setup(driver_setup_observation{}) ==
            driver_setup_state::unknown);
}

TEST_CASE("driver setup: the description names each piece and the code")
{
    const std::string text = describe_driver_setup(
        seen(driver_fact::yes, helper_service_state::running,
             helper_probe::access_denied, 5));
    REQUIRE(text.find("driver package: installed") != std::string::npos);
    REQUIRE(text.find("helper service: running") != std::string::npos);
    REQUIRE(text.find("helper pipe: access denied") != std::string::npos);
    REQUIRE(text.find("Win32 error 5") != std::string::npos);

    // No code, no mention of one.
    const std::string quiet = describe_driver_setup(
        seen(driver_fact::no, helper_service_state::not_installed,
             helper_probe::no_pipe));
    REQUIRE(quiet.find("Win32 error") == std::string::npos);
    REQUIRE(quiet.find("driver package: not installed") != std::string::npos);
}

TEST_CASE("driver setup: the probe answers on this host without making a port")
{
    const driver_setup_report report = probe_driver_setup();
    // Whatever the host has, the probe names a state and says what it saw.
    REQUIRE(!report.detail.empty());
    REQUIRE(report.state == classify_driver_setup(report.seen) ||
            report.state == driver_setup_state::not_applicable);
}
