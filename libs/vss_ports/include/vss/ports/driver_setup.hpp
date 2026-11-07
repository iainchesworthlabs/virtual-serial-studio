//
// Copyright (c) 2026 Virtual Serial Studio contributors
//
// SPDX-License-Identifier: GPL-3.0-or-later
//
// Whether this PC is set up to make virtual COM ports, and if not, which piece
// is missing.
//
// A Windows port needs three things in place: the driver package, the helper
// service that makes ports for ordinary users, and permission for the signed-in
// user to talk to that service. When one is missing every request fails the
// same way ("permission denied", "unsupported", "I/O error") and the person at
// the keyboard cannot tell which. `probe_driver_setup()` asks each piece
// separately, without making a port, and `classify_driver_setup()` names the
// first one that is wrong. The classification is plain code over an observation,
// so every outcome is tested on every platform.
//

#pragma once

#include <string>

namespace vss
{

/// What is wrong, in the order a person fixes it.
enum class driver_setup_state
{
    /// This operating system needs no driver (Linux, macOS).
    not_applicable,
    /// The driver, the helper and the access are all in place.
    ready,
    /// The driver package is not installed.
    driver_missing,
    /// The helper service is not installed.
    helper_missing,
    /// The helper service is installed and not running (or not answering).
    helper_stopped,
    /// The helper answered, but it is older than this program or speaks another
    /// protocol.
    helper_incompatible,
    /// The helper is there and refused this user.
    helper_denied,
    /// Something was asked and the answer was not understood.
    unknown
};

/// A yes or no that may not be known.
enum class driver_fact
{
    unknown,
    no,
    yes
};

enum class helper_service_state
{
    unknown,
    not_installed,
    stopped,
    running
};

/// How the helper answered a probe on its pipe.
enum class helper_probe
{
    /// No probe was made.
    not_asked,
    /// The pipe does not exist: the service is not running.
    no_pipe,
    /// The system refused to open the pipe, or the helper could not tell who
    /// the caller is.
    access_denied,
    /// The pipe opened and the exchange failed for another reason.
    failed,
    /// The helper answered, and not with what this program speaks.
    incompatible,
    /// The helper answered as expected.
    ok
};

/// What the host said, piece by piece.
struct driver_setup_observation
{
    /// The driver package is installed: its control device or a port is there,
    /// or the driver store lists it.
    driver_fact driver_installed = driver_fact::unknown;
    /// The kernel-mode driver's control device is present. Ports then come from
    /// it and need no helper.
    bool control_device = false;
    helper_service_state service = helper_service_state::unknown;
    helper_probe pipe = helper_probe::not_asked;
    /// The Win32 error behind `service` or `pipe`, or 0. Shown in the details,
    /// never in a sentence for the person.
    unsigned long code = 0;
};

struct driver_setup_report
{
    driver_setup_state state = driver_setup_state::unknown;
    driver_setup_observation seen;
    /// One English line with the observation, for the log and the details.
    std::string detail;
};

/// Names the first piece that is wrong, or `ready`. Pure.
driver_setup_state
classify_driver_setup(const driver_setup_observation& seen) noexcept;

/// The observation as one line of English, for a log or a "details" box:
/// `driver package: installed; service: running; helper pipe: access denied
/// (error 5)`.
std::string describe_driver_setup(const driver_setup_observation& seen);

/// Reads the host: lists the driver package, asks the service control manager
/// and sends the helper one request that changes nothing. Makes no port. Takes
/// a moment when the helper's pipe is busy, so call it off the GUI thread where
/// that matters. `not_applicable` everywhere but Windows.
driver_setup_report probe_driver_setup();

/// The last step of making a port that failed in this process, and the Win32
/// error behind it. Kept so a "permission denied" can say which call it came
/// from. Empty when nothing has failed.
struct driver_failure
{
    /// A short English phrase: "open the helper pipe", "reply of the helper".
    std::string step;
    unsigned long code = 0;
};

driver_failure last_driver_failure();

} // namespace vss
