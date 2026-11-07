//
// Copyright (c) 2026 Virtual Serial Studio contributors
//
// SPDX-License-Identifier: GPL-3.0-or-later
//
// "Set up Virtual Serial Studio": what each way the set-up can end says, what
// the Setup check shows for each missing piece, and the dialog that runs the
// step. The step is injected, so nothing here installs anything or shows a
// permission prompt.
//

#include "test_framework.hpp"

#include "backend/driver_error.hpp"
#include "backend/fake_studio_backend.hpp"
#include "support/driver_setup_dialog.hpp"
#include "support/elevated_step.hpp"
#include "support/setup_facts.hpp"
#include "widgets/notes.hpp"

#include "qt_wait.hpp"

#include <QPushButton>

namespace
{

namespace support = vss::gui::support;
namespace perm = vss::permissions;
using vss::gui::driver_problem;

perm::step_result ended(int exit_code)
{
    perm::step_result result;
    result.status = exit_code == 0 ? perm::step_status::done
                                   : perm::step_status::failed;
    result.exit_code = exit_code;
    return result;
}

vss::gui::setup_facts windows_facts(driver_problem problem,
                                    bool driver_found = true)
{
    vss::gui::setup_facts facts;
    facts.platform = QStringLiteral("windows");
    facts.driver_found = driver_found;
    facts.driver.problem = problem;
    return facts;
}

support::platform_facts known_platform()
{
    support::platform_facts platform;
    platform.elevation_known = true;
    platform.elevated = false;
    return platform;
}

} // namespace

TEST_CASE("driver setup: every way the script can end has its own sentence")
{
    using support::driver_setup_result;
    struct expectation
    {
        int exit_code;
        driver_setup_result result;
    };
    const expectation table[] = {
        {0, driver_setup_result::done},
        {10, driver_setup_result::done_restart},
        {2, driver_setup_result::not_elevated},
        {6, driver_setup_result::untrusted},
        {7, driver_setup_result::driver_failed},
        {8, driver_setup_result::helper_failed},
        {9, driver_setup_result::incomplete},
        {1, driver_setup_result::failed},
        {-1, driver_setup_result::failed},
        {4242, driver_setup_result::failed}};
    QStringList sentences;
    for (const expectation& row : table)
    {
        const auto report =
            support::report_for_driver_setup(ended(row.exit_code));
        REQUIRE(report.result == row.result);
        REQUIRE_EQ(report.exit_code, row.exit_code);
        REQUIRE(!report.message.isEmpty());
        // Failed twice with different codes reads the same, the rest do not.
        if (row.result != driver_setup_result::failed)
        {
            REQUIRE(!sentences.contains(report.message));
            sentences << report.message;
        }
    }

    perm::step_result declined;
    declined.status = perm::step_status::declined;
    const auto no = support::report_for_driver_setup(declined);
    REQUIRE(no.result == driver_setup_result::declined);
    REQUIRE(no.message.contains(QStringLiteral("nothing was changed")));

    perm::step_result none;
    none.status = perm::step_status::unsupported;
    REQUIRE(support::report_for_driver_setup(none).result ==
            driver_setup_result::unavailable);
}

TEST_CASE("driver setup: an unsigned build is told why and what to do")
{
    const auto report = support::report_for_driver_setup(ended(6));
    REQUIRE(report.result == support::driver_setup_result::untrusted);
    REQUIRE(report.message.contains(QStringLiteral("nothing was installed")));
    REQUIRE(report.message.contains(QStringLiteral("test signing")));
    REQUIRE(!report.installed());
    REQUIRE(support::report_for_driver_setup(ended(0)).installed());
    REQUIRE(support::report_for_driver_setup(ended(10)).installed());
}

TEST_CASE("driver setup: each missing piece is a row with the one action")
{
    const support::platform_facts platform = known_platform();
    const QStringList devices = {QStringLiteral("COM3")};

    for (const driver_problem problem :
         {driver_problem::driver_missing, driver_problem::helper_missing,
          driver_problem::helper_stopped, driver_problem::helper_incompatible,
          driver_problem::helper_denied})
    {
        const auto report = support::build_setup_report(
            windows_facts(problem, problem != driver_problem::driver_missing),
            platform, devices);
        REQUIRE(!report.ports_ready);
        REQUIRE(report.problem == problem);
        REQUIRE(report.state ==
                support::setup_report::readiness::needs_attention);
        const auto& row = report.rows.at(0);
        REQUIRE_EQ(row.chip, QStringLiteral("Needs attention"));
        REQUIRE_EQ(row.action_text,
                   QStringLiteral("Set up Virtual Serial Studio"));
        REQUIRE(row.action == support::setup_action::setup_driver);
        REQUIRE_EQ(row.sentence, vss::gui::driver_problem_body(problem));
        REQUIRE(row.highlight);
    }

    // Nothing wrong: the plain row, no action.
    const auto fine = support::build_setup_report(
        windows_facts(driver_problem::none), platform, devices);
    REQUIRE(fine.ports_ready);
    REQUIRE(fine.problem == driver_problem::none);
    REQUIRE(fine.rows.at(0).action == support::setup_action::none);

    // A problem the set-up cannot fix is not offered it.
    const auto other = support::build_setup_report(
        windows_facts(driver_problem::unknown), platform, devices);
    REQUIRE(other.problem == driver_problem::none);
    REQUIRE(other.rows.at(0).action != support::setup_action::setup_driver);
}

TEST_CASE("driver setup: com0com still counts as ready beside a driver problem")
{
    vss::gui::setup_facts facts = windows_facts(driver_problem::helper_stopped);
    facts.com0com_found = true;
    facts.com0com_pairs = 2;
    const auto report = support::build_setup_report(
        facts, known_platform(), {QStringLiteral("COM3")});
    REQUIRE(report.ports_ready);
    // The row is the com0com one, not an alarm.
    REQUIRE(report.rows.at(0).action != support::setup_action::setup_driver);
}

TEST_CASE("driver setup dialog: nothing runs until the button is pressed")
{
    vss::gui::FakeStudioBackend backend;
    backend.set_facts(windows_facts(driver_problem::driver_missing, false));
    support::DriverSetupDialog dialog(backend, driver_problem::driver_missing);
    int runs = 0;
    dialog.set_runner(
        [&runs]
        {
            ++runs;
            return support::report_for_driver_setup(ended(0));
        },
        true);
    dialog.show();
    vss_test::spin(20);
    REQUIRE_EQ(runs, 0);
    REQUIRE(dialog.setup_button()->isEnabled());
    REQUIRE_EQ(dialog.setup_button()->text(),
               QStringLiteral("Set up Virtual Serial Studio"));
    REQUIRE_EQ(dialog.setup_button()->accessibleName(),
               QStringLiteral("Set up Virtual Serial Studio"));
    REQUIRE(!dialog.result_note()->isVisible());
}

TEST_CASE("driver setup dialog: a finished set-up is checked again and reported")
{
    vss::gui::FakeStudioBackend backend;
    backend.set_facts(windows_facts(driver_problem::driver_missing, false));
    support::DriverSetupDialog dialog(backend, driver_problem::driver_missing);
    dialog.set_runner(
        [&backend]
        {
            // The step fixes the machine: the next check finds nothing wrong.
            backend.set_facts(windows_facts(driver_problem::none));
            return support::report_for_driver_setup(ended(0));
        },
        true);
    dialog.show();
    bool finished = false;
    QObject::connect(&dialog, &support::DriverSetupDialog::set_up_finished,
                     [&finished] { finished = true; });
    dialog.begin();
    // While it runs the window answers and cannot be closed under it.
    REQUIRE(dialog.is_running() || finished);
    REQUIRE(vss_test::wait_until([&] { return !dialog.is_running(); }));
    REQUIRE(finished);
    REQUIRE(dialog.is_ready());
    REQUIRE(dialog.result_note()->isVisible());
    REQUIRE(dialog.result_note()->text().contains(
        QStringLiteral("is set up")));
    // Nothing left for the button to do.
    REQUIRE(!dialog.setup_button()->isEnabled());
}

TEST_CASE("driver setup dialog: a script that says yes is not believed over the check")
{
    vss::gui::FakeStudioBackend backend;
    backend.set_facts(
        windows_facts(driver_problem::helper_stopped));
    support::DriverSetupDialog dialog(backend, driver_problem::helper_stopped);
    dialog.set_runner([] { return support::report_for_driver_setup(ended(0)); },
                      true);
    dialog.show();
    dialog.begin();
    REQUIRE(vss_test::wait_until([&] { return !dialog.is_running(); }));
    REQUIRE(!dialog.is_ready());
    REQUIRE(dialog.result_note()->text().contains(
        QStringLiteral("still cannot make ports")));
    // Another try is allowed.
    REQUIRE(dialog.setup_button()->isEnabled());
}

TEST_CASE("driver setup dialog: a declined prompt changes nothing and can be retried")
{
    vss::gui::FakeStudioBackend backend;
    backend.set_facts(windows_facts(driver_problem::driver_missing, false));
    support::DriverSetupDialog dialog(backend, driver_problem::driver_missing);
    dialog.set_runner(
        []
        {
            perm::step_result declined;
            declined.status = perm::step_status::declined;
            return support::report_for_driver_setup(declined);
        },
        true);
    dialog.show();
    dialog.begin();
    REQUIRE(vss_test::wait_until([&] { return !dialog.is_running(); }));
    REQUIRE(!dialog.is_ready());
    REQUIRE(dialog.result_note()->text().contains(
        QStringLiteral("declined the Windows permission prompt")));
    REQUIRE(dialog.setup_button()->isEnabled());
}

TEST_CASE("driver setup dialog: without the driver files it explains and offers no button")
{
    vss::gui::FakeStudioBackend backend;
    support::DriverSetupDialog dialog(backend, driver_problem::driver_missing);
    int runs = 0;
    dialog.set_runner(
        [&runs]
        {
            ++runs;
            return support::report_for_driver_setup(ended(0));
        },
        false);
    dialog.show();
    dialog.begin();
    vss_test::spin(20);
    REQUIRE_EQ(runs, 0);
    REQUIRE(!dialog.setup_button()->isEnabled());
}
