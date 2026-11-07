//
// Copyright (c) 2026 Virtual Serial Studio contributors
//
// SPDX-License-Identifier: GPL-3.0-or-later
//

#include "backend/driver_error.hpp"

#include <vss/core/profile.hpp>
#include <vss/ports/driver_setup.hpp>

#include <QObject>

#include <mutex>
#include <string>
#include <utility>

namespace vss::gui
{
namespace
{

class driver_category final : public boost::system::error_category
{
public:
    const char* name() const noexcept override { return "vss_gui.driver"; }

    std::string message(int value) const override
    {
        // The sentence is translated and the category must return a narrow
        // string, so it goes through UTF-8, which the callers decode again.
        return driver_problem_body(static_cast<driver_problem>(value))
            .toUtf8()
            .toStdString();
    }
};

std::mutex details_mutex;
QString details_text;

void remember_details(const driver_check& check, const vss::error_code& failure)
{
    QString text = check.details;
    if (failure)
    {
        text += QStringLiteral("\nengine: %1 (%2:%3)")
                    .arg(QString::fromStdString(failure.message()),
                         QString::fromLatin1(failure.category().name()))
                    .arg(failure.value());
    }
    const vss::driver_failure last = vss::last_driver_failure();
    if (!last.step.empty())
    {
        text += QStringLiteral("\nfailed step: %1 (Win32 error %2)")
                    .arg(QString::fromStdString(last.step))
                    .arg(last.code);
    }
    const std::lock_guard lock(details_mutex);
    details_text = std::move(text);
}

bool is_candidate(const vss::error_code& failure)
{
    return failure == vss::errc::permission_denied ||
           failure == vss::errc::unsupported ||
           failure == vss::errc::io_error || failure == vss::errc::timeout;
}

} // namespace

const boost::system::error_category& driver_error_category() noexcept
{
    static const driver_category category;
    return category;
}

vss::error_code make_driver_error(driver_problem problem)
{
    return {static_cast<int>(problem), driver_error_category()};
}

std::optional<driver_problem> driver_problem_of(const vss::error_code& error)
{
    if (error.category() != driver_error_category())
        return std::nullopt;
    return static_cast<driver_problem>(error.value());
}

bool driver_problem_offers_setup(driver_problem problem)
{
    switch (problem)
    {
    case driver_problem::driver_missing:
    case driver_problem::helper_missing:
    case driver_problem::helper_stopped:
    case driver_problem::helper_incompatible:
    case driver_problem::helper_denied:
        return true;
    case driver_problem::none:
    case driver_problem::unknown:
        break;
    }
    return false;
}

QString driver_problem_title(driver_problem problem)
{
    switch (problem)
    {
    case driver_problem::driver_missing:
        return QObject::tr("The Virtual Serial Studio driver is not installed");
    case driver_problem::helper_missing:
        return QObject::tr("The Virtual Serial Studio helper is not installed");
    case driver_problem::helper_stopped:
        return QObject::tr("The Virtual Serial Studio helper is not running");
    case driver_problem::helper_incompatible:
        return QObject::tr("The Virtual Serial Studio helper is out of date");
    case driver_problem::helper_denied:
        return QObject::tr(
            "Your account cannot use the Virtual Serial Studio helper");
    case driver_problem::none:
    case driver_problem::unknown:
        break;
    }
    return QObject::tr("Windows would not make the port");
}

QString driver_problem_body(driver_problem problem)
{
    switch (problem)
    {
    case driver_problem::driver_missing:
        return QObject::tr(
            "Windows needs the Virtual Serial Studio driver to make a COM "
            "port that other programs can open, and this PC does not have it "
            "yet. Nothing was changed. Choose Set up Virtual Serial Studio to "
            "install it.");
    case driver_problem::helper_missing:
        return QObject::tr(
            "The helper service that makes ports without asking for "
            "administrator rights each time is not installed. Nothing was "
            "changed. Choose Set up Virtual Serial Studio to install it.");
    case driver_problem::helper_stopped:
        return QObject::tr(
            "The helper service that makes ports is installed but not "
            "running. Nothing was changed. Choose Set up Virtual Serial "
            "Studio to start it again.");
    case driver_problem::helper_incompatible:
        return QObject::tr(
            "The helper service on this PC is older than this program, so it "
            "cannot make ports. Nothing was changed. Choose Set up Virtual "
            "Serial Studio to update it.");
    case driver_problem::helper_denied:
        return QObject::tr(
            "Windows did not let your account use the helper service that "
            "makes ports. Nothing was changed. Choose Set up Virtual Serial "
            "Studio to repair it, or ask an administrator of this PC.");
    case driver_problem::none:
    case driver_problem::unknown:
        break;
    }
    return QObject::tr(
        "Windows would not make the port, and Studio could not tell which "
        "part is at fault. Nothing was changed. Open Tools, Setup check to "
        "see what is missing, then copy the details if you ask for help.");
}

vss::error_code driver_aware_error(const StudioBackend& backend,
                                   const vss::error_code& failure)
{
    VSS_PROFILE_ZONE("gui.backend.driver_aware_error");
    if (!is_candidate(failure))
        return failure;

    const driver_check check = backend.check_driver();
    remember_details(check, failure);
    switch (check.problem)
    {
    case driver_problem::none:
        break;
    case driver_problem::unknown:
        // The bare refusal is the one that says nothing; say that it could not
        // be traced, and keep the details. Any other code is left as it was.
        if (failure == vss::errc::permission_denied)
            return make_driver_error(driver_problem::unknown);
        break;
    default:
        return make_driver_error(check.problem);
    }
    // The set-up looks right, so the refusal is something else; a "permission
    // denied" is still said in words with the details behind it.
    if (failure == vss::errc::permission_denied)
        return make_driver_error(driver_problem::unknown);
    return failure;
}

QString last_driver_details()
{
    const std::lock_guard lock(details_mutex);
    return details_text;
}

} // namespace vss::gui
