//
// Copyright (c) 2026 Virtual Serial Studio contributors
//
// SPDX-License-Identifier: GPL-3.0-or-later
//
// Licences for the tests of the Licence page, the edition chip and the
// notices: one status for each state, judged by the licence library itself so
// the tests use the same days_left and grace_ends the program does.
//

#pragma once

#include "backend/licence_view.hpp"
#include "support/licence_text.hpp"

#include <vss/license/license.hpp>

#include <QString>

#include <chrono>
#include <string>
#include <string_view>

namespace vss::gui::test_licence
{

namespace lic = vss::license;

inline lic::time_point at(std::string_view text)
{
    return *lic::parse_time(text);
}

/// The date the screens show for a time. The form itself (the locale's long
/// form, local time, no day of the week) has its own test.
inline QString date(std::string_view text)
{
    return vss::gui::support::licence_date(at(text));
}

/// An annual Pro licence for 13 November 2026 to 13 November 2027, with 28
/// days of grace.
inline lic::licence annual()
{
    lic::licence value;
    value.licence_id = "VSS-2026-000123";
    value.kind = lic::licence_kind::annual;
    value.edition = lic::edition::pro;
    value.licensee_name = "Example Lab Pty Ltd";
    value.issued_at = at("2026-09-01T00:00:00Z");
    value.starts_at = at("2026-11-13T00:00:00Z");
    value.expires_at = at("2027-11-13T00:00:00Z");
    value.grace_days = 28;
    value.seats = 3;
    value.features = {"plugins.official", "driver.signed", "support"};
    return value;
}

/// A 14 day trial.
inline lic::licence trial()
{
    lic::licence value = annual();
    value.licence_id = "VSS-TRIAL-000007";
    value.kind = lic::licence_kind::trial;
    value.starts_at = at("2027-03-01T00:00:00Z");
    value.expires_at = at("2027-03-15T00:00:00Z");
    value.grace_days = 0;
    value.seats = 1;
    return value;
}

inline lic::status judge(const lic::licence& value, std::string_view now,
                         std::string_view latest_seen = {},
                         const std::string& computer = "LAB-RIG-07")
{
    lic::context where;
    where.now = at(now);
    if (!latest_seen.empty())
        where.latest_seen = at(latest_seen);
    where.computer_name = computer;
    return lic::evaluate(value, nullptr, where);
}

/// The status of `value` with `days` whole days left before it ends.
inline lic::status judge_days_left(const lic::licence& value, int days)
{
    lic::context where;
    where.computer_name = "LAB-RIG-07";
    where.now = value.expires_at - std::chrono::hours{24 * days + 6};
    return lic::evaluate(value, nullptr, where);
}

inline lic::status valid_status()
{
    return judge(annual(), "2027-03-01T12:00:00Z");
}
inline lic::status grace_status()
{
    return judge(annual(), "2027-11-20T00:00:00Z");
}
inline lic::status expired_status()
{
    return judge(annual(), "2028-02-01T00:00:00Z");
}
inline lic::status not_yet_valid_status()
{
    return judge(annual(), "2026-10-01T00:00:00Z");
}
inline lic::status clock_suspect_status()
{
    return judge(annual(), "2027-03-01T12:00:00Z", "2028-01-01T00:00:00Z");
}
inline lic::status wrong_computer_status()
{
    lic::licence value = annual();
    value.machines = {"STUDIO-*"};
    return judge(value, "2027-03-01T12:00:00Z");
}

/// A status that failed before it could be read, with only a reason.
inline lic::status unreadable_status(lic::reason reason)
{
    lic::status value;
    value.state = lic::state::invalid;
    value.reason = reason;
    return value;
}

/// The view a backend reports for `status`: one per-user file in use.
inline vss::gui::licence_view
view_of(const lic::status& status, bool machine_wide = false,
        const QString& file_name = QStringLiteral("VSS-2026-000123."
                                                  "vsslic"))
{
    vss::gui::licence_view view;
    view.status = status;
    view.computer_name = QStringLiteral("LAB-RIG-07");
    vss::gui::licence_file_view file;
    file.status = status;
    file.file_name = file_name;
    file.machine_wide = machine_wide;
    file.in_use = true;
    view.files.push_back(file);
    return view;
}

} // namespace vss::gui::test_licence
