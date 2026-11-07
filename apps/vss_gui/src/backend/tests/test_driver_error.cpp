//
// Copyright (c) 2026 Virtual Serial Studio contributors
//
// SPDX-License-Identifier: GPL-3.0-or-later
//
// What a refused port says. The engine answers a missing driver, a stopped
// helper, an old helper and a refused user with a few shared codes; these tests
// check that the check behind the fake backend turns each into its own sentence,
// and that no sentence is the bare code.
//

#include "backend/driver_error.hpp"
#include "backend/fake_studio_backend.hpp"

#include "test_framework.hpp"

#include <QString>

#include <vector>

using namespace vss::gui;

namespace
{

void say(FakeStudioBackend& backend, driver_problem problem)
{
    setup_facts facts;
    facts.platform = QStringLiteral("windows");
    facts.driver.problem = problem;
    facts.driver.details = QStringLiteral("driver package: test; code 5");
    backend.set_facts(facts);
}

/// The problems the guided setup fixes. A function, not a global: a vector built
/// at start-up could throw where nothing can catch it.
std::vector<driver_problem> setup_problem_list()
{
    return {driver_problem::driver_missing, driver_problem::helper_missing,
            driver_problem::helper_stopped,
            driver_problem::helper_incompatible,
            driver_problem::helper_denied};
}

} // namespace

TEST_CASE("driver error: each set-up problem has its own sentence and next step")
{
    QStringList bodies;
    for (const driver_problem problem : setup_problem_list())
    {
        const QString body = driver_problem_body(problem);
        // A next step, and the action that does it.
        REQUIRE(body.contains(QStringLiteral("Set up Virtual Serial Studio")));
        REQUIRE(body.contains(QStringLiteral("Nothing was changed")));
        REQUIRE(!bodies.contains(body));
        bodies << body;
        REQUIRE(driver_problem_offers_setup(problem));
        REQUIRE(!driver_problem_title(problem).isEmpty());
    }
    // "Something else" has no set-up to offer, and says where to look.
    REQUIRE(!driver_problem_offers_setup(driver_problem::unknown));
    REQUIRE(driver_problem_body(driver_problem::unknown)
                .contains(QStringLiteral("Setup check")));
}

TEST_CASE("driver error: a refused port is explained by what the check finds")
{
    for (const driver_problem problem : setup_problem_list())
    {
        FakeStudioBackend backend;
        say(backend, problem);
        for (const auto code :
             {vss::errc::permission_denied, vss::errc::unsupported,
              vss::errc::io_error, vss::errc::timeout})
        {
            const auto error = driver_aware_error(
                backend, vss::make_error_code(code));
            const auto found = driver_problem_of(error);
            REQUIRE(found.has_value());
            REQUIRE(*found == problem);
            // The text a screen shows is the sentence, never the code.
            REQUIRE_EQ(error_text(error), driver_problem_body(problem));
            REQUIRE(!error_text(error).contains(
                QStringLiteral("permission denied")));
        }
    }
}

TEST_CASE("driver error: the details carry what was seen, for Copy details")
{
    FakeStudioBackend backend;
    say(backend, driver_problem::helper_denied);
    static_cast<void>(driver_aware_error(
        backend, vss::make_error_code(vss::errc::permission_denied)));
    const QString details = last_driver_details();
    REQUIRE(details.contains(QStringLiteral("driver package: test; code 5")));
    REQUIRE(details.contains(QStringLiteral("permission denied")));
}

TEST_CASE("driver error: a refusal with nothing wrong in the set-up is not bare")
{
    FakeStudioBackend backend;
    say(backend, driver_problem::none);
    const auto denied = driver_aware_error(
        backend, vss::make_error_code(vss::errc::permission_denied));
    const auto found = driver_problem_of(denied);
    REQUIRE(found.has_value());
    REQUIRE(*found == driver_problem::unknown);
    REQUIRE(error_text(denied).contains(QStringLiteral("Setup check")));
    REQUIRE(!driver_problem_offers_setup(*found));

    // The codes that mean what they say are left alone.
    for (const auto code :
         {vss::errc::unsupported, vss::errc::io_error, vss::errc::timeout})
    {
        const auto error =
            driver_aware_error(backend, vss::make_error_code(code));
        REQUIRE(!driver_problem_of(error).has_value());
        REQUIRE(error == code);
    }
}

TEST_CASE("driver error: errors that are not about the set-up pass through")
{
    FakeStudioBackend backend;
    say(backend, driver_problem::driver_missing);
    for (const auto code :
         {vss::errc::already_exists, vss::errc::device_busy,
          vss::errc::port_exhausted, vss::errc::invalid_argument,
          vss::errc::not_found})
    {
        const auto error =
            driver_aware_error(backend, vss::make_error_code(code));
        REQUIRE(!driver_problem_of(error).has_value());
        REQUIRE(error == code);
    }
}

TEST_CASE("driver error: a check that could not be understood names only a refusal")
{
    FakeStudioBackend backend;
    say(backend, driver_problem::unknown);
    REQUIRE(driver_problem_of(driver_aware_error(
                backend,
                vss::make_error_code(vss::errc::permission_denied))) ==
            driver_problem::unknown);
    const auto io = driver_aware_error(
        backend, vss::make_error_code(vss::errc::io_error));
    REQUIRE(!driver_problem_of(io).has_value());
}

TEST_CASE("driver error: an error of another category is not a driver problem")
{
    REQUIRE(!driver_problem_of(vss::make_error_code(vss::errc::io_error))
                 .has_value());
    REQUIRE(driver_problem_of(make_driver_error(driver_problem::helper_stopped)) ==
            driver_problem::helper_stopped);
}
