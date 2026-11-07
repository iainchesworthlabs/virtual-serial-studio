//
// Copyright (c) 2026 Virtual Serial Studio contributors
//
// SPDX-License-Identifier: GPL-3.0-or-later
//

#include <vss/core/profile.hpp>
#include <vss/ports/driver_setup.hpp>

#include "platform/vcom_driver_platform.hpp"

#include <string>

namespace vss
{

driver_setup_state
classify_driver_setup(const driver_setup_observation& seen) noexcept
{
    // The kernel-mode driver makes its ports itself; the helper is only for COM
    // numbers there.
    if (seen.control_device)
        return driver_setup_state::ready;

    // The first thing a new user lacks is the driver, and the guided set-up
    // installs both, so it comes first.
    if (seen.driver_installed == driver_fact::no)
        return driver_setup_state::driver_missing;

    switch (seen.pipe)
    {
    case helper_probe::ok:
        return driver_setup_state::ready;
    case helper_probe::incompatible:
        return driver_setup_state::helper_incompatible;
    case helper_probe::access_denied:
        return driver_setup_state::helper_denied;
    case helper_probe::no_pipe:
        return seen.service == helper_service_state::not_installed
                   ? driver_setup_state::helper_missing
                   : driver_setup_state::helper_stopped;
    case helper_probe::failed:
    case helper_probe::not_asked:
        break;
    }
    return driver_setup_state::unknown;
}

namespace
{

const char* text_of(driver_fact fact) noexcept
{
    switch (fact)
    {
    case driver_fact::yes:
        return "installed";
    case driver_fact::no:
        return "not installed";
    case driver_fact::unknown:
        break;
    }
    return "not known";
}

const char* text_of(helper_service_state state) noexcept
{
    switch (state)
    {
    case helper_service_state::not_installed:
        return "not installed";
    case helper_service_state::stopped:
        return "installed, not running";
    case helper_service_state::running:
        return "running";
    case helper_service_state::unknown:
        break;
    }
    return "not known";
}

const char* text_of(helper_probe probe) noexcept
{
    switch (probe)
    {
    case helper_probe::not_asked:
        return "not asked";
    case helper_probe::no_pipe:
        return "no pipe";
    case helper_probe::access_denied:
        return "access denied";
    case helper_probe::failed:
        return "failed";
    case helper_probe::incompatible:
        return "incompatible version";
    case helper_probe::ok:
        break;
    }
    return "answered";
}

} // namespace

std::string describe_driver_setup(const driver_setup_observation& seen)
{
    std::string text = "driver package: ";
    text += seen.control_device ? "installed (control device present)"
                                : text_of(seen.driver_installed);
    text += "; helper service: ";
    text += text_of(seen.service);
    text += "; helper pipe: ";
    text += text_of(seen.pipe);
    if (seen.code != 0)
        text += " (Win32 error " + std::to_string(seen.code) + ")";
    return text;
}

driver_setup_report probe_driver_setup()
{
    VSS_PROFILE_ZONE("ports.driver_setup.probe");
    driver_setup_report report;
    const auto seen = detail::platform_observe_driver_setup();
    if (!seen)
    {
        report.state = driver_setup_state::not_applicable;
        report.detail = "this system needs no driver";
        return report;
    }
    report.seen = *seen;
    report.state = classify_driver_setup(report.seen);
    report.detail = describe_driver_setup(report.seen);
    return report;
}

driver_failure last_driver_failure()
{
    return detail::platform_last_driver_failure();
}

} // namespace vss
