//
// Copyright (c) 2026 Virtual Serial Studio contributors
//
// SPDX-License-Identifier: GPL-3.0-or-later
//
// Why a port could not be made on a PC where the driver is not set up, in words
// a person can act on.
//
// The engine answers a missing driver, a stopped helper service, an old helper
// and a refused user with the same few codes ("permission denied",
// "unsupported", "I/O error"). A screen that shows that code tells the person
// nothing. `driver_aware_error` asks the backend what is actually wrong and, if
// it is one of the set-up problems, returns an error in this category whose
// message is the sentence and the next step. Every screen that already shows
// `error_text()` then shows it, and the ones that can offer the guided set-up
// ask `driver_problem_of()`.
//

#pragma once

#include "backend/studio_backend.hpp"

#include <vss/core/error.hpp>

#include <QString>

#include <optional>

namespace vss::gui
{

/// The category of the errors made by `make_driver_error`.
const boost::system::error_category& driver_error_category() noexcept;

vss::error_code make_driver_error(driver_problem problem);

/// The problem an error stands for, or nothing if it is any other error.
std::optional<driver_problem> driver_problem_of(const vss::error_code& error);

/// True when the guided set-up fixes the problem: the driver or the helper is
/// missing, stopped, old, or refusing this user.
bool driver_problem_offers_setup(driver_problem problem);

/// "Virtual Serial Studio's driver is not installed": the short heading.
QString driver_problem_title(driver_problem problem);

/// The sentence for the person and the next step. Never an engine code.
QString driver_problem_body(driver_problem problem);

/// If a port could not be made, and the driver set-up is why, the error that
/// says so; otherwise `failure` as it was. Asks the backend for a fresh check,
/// which sends the helper one harmless request, so call it only for a failure
/// to make a driver port, and not on the hot path. Also records the details
/// (what the check saw, the engine's own code, and the call that failed) for
/// `last_driver_details()`.
vss::error_code driver_aware_error(const StudioBackend& backend,
                                   const vss::error_code& failure);

/// The details of the most recent `driver_aware_error`: one block of English for
/// "Copy details" and the log. Empty before the first.
QString last_driver_details();

} // namespace vss::gui
