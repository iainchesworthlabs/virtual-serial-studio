//
// Copyright (c) 2026 Virtual Serial Studio contributors
//
// SPDX-License-Identifier: GPL-3.0-or-later
//

#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace vss::control
{

/// How one check of a run ended.
enum class case_outcome
{
    passed,
    /// What was checked for did not happen.
    failed,
    /// The check itself could not be made.
    error,
    skipped
};

/// The name of an outcome on the wire and in the audit log: `passed`.
std::string_view outcome_name(case_outcome value) noexcept;

/// One check of a run.
struct junit_case
{
    std::string name;
    case_outcome outcome = case_outcome::passed;
    /// Why it failed or was skipped.  Text from outside; it is made safe
    /// when the report is written.
    std::string message;
    std::uint64_t duration_ms = 0;
};

/// A run, as the report shows it.
struct junit_suite
{
    std::string name;
    /// When it began: milliseconds since 1970-01-01 UTC.
    std::uint64_t started_ms = 0;
    std::uint64_t duration_ms = 0;
    std::vector<junit_case> cases;
};

/// The run as a JUnit XML document: one `testsuite` in a `testsuites`.
///
/// Every name and message comes from a client, so nothing of them reaches
/// the document as it came: the five characters XML gives a meaning are
/// written as entities, bytes that are not valid UTF-8 become `?`, and the
/// characters XML 1.0 does not allow at all are left out.  No text of a
/// client can end an attribute, start an element or name an entity.
std::string write_junit(const junit_suite& suite);

/// `text` made safe to stand in an XML attribute or between two tags, as
/// `write_junit` does it.
std::string xml_text(std::string_view text);

/// A time of day as XML Schema writes it: `2026-11-13T09:30:00Z`.
std::string iso_time(std::uint64_t milliseconds_since_1970);

} // namespace vss::control
