//
// Copyright (c) 2026 Virtual Serial Studio contributors
//
// SPDX-License-Identifier: GPL-3.0-or-later
//
// The first run on a PC where nothing is installed, and the same piece by piece:
// no driver, no helper service, a stopped one, an old one, one that refuses the
// user. Each journey starts the window as a person starts the program, reads
// what the first screen says, chooses "Set up Virtual Serial Studio" and reads
// what the dialog promises. The set-up itself is never run: the button is read
// and never pressed, because it would show a Windows permission prompt.
//

#include "simple_steps.hpp"

#include "backend/driver_error.hpp"
#include "qt_input.hpp"
#include "qt_wait.hpp"
#include "support/driver_setup_dialog.hpp"
#include "widgets/notes.hpp"

#include <QLabel>

#include <string>

namespace
{

using namespace vss::gui;
using namespace vss::gui::ui;
namespace st = vss::gui::ui::steps;
using vss::gui::simple::job;

struct piece
{
    driver_problem problem;
    const char* name;
};

const piece kPieces[] = {
    {driver_problem::driver_missing, "the driver is not installed"},
    {driver_problem::helper_missing, "the helper service is not installed"},
    {driver_problem::helper_stopped, "the helper service is not running"},
    {driver_problem::helper_incompatible, "the helper service is too old"},
    {driver_problem::helper_denied, "the helper refuses this user"}};

QString set_up_text()
{
    return QStringLiteral("Set up Virtual Serial Studio");
}

/// What the machine tells the window about itself.
void tell(Session& s, driver_problem problem)
{
    setup_facts facts = s.fake().facts();
    facts.platform = QStringLiteral("windows");
    facts.driver_found = problem != driver_problem::driver_missing;
    facts.driver.problem = problem;
    facts.driver.details =
        QStringLiteral("driver package: test; helper pipe: test (Win32 error 5)");
    s.fake().set_facts(facts);
}

/// The note on the page that carries a button of this text.
NoteBanner* note_with_button(QWidget* root, const QString& text)
{
    for (auto* note : root->findChildren<NoteBanner*>())
    {
        if (note->isVisibleTo(root) && note->action_button() != nullptr &&
            note->action_button()->isVisibleTo(root) &&
            note->action_button()->text() == text)
            return note;
    }
    return nullptr;
}

bool has_label(QWidget* root, const QString& text)
{
    for (auto* label : root->findChildren<QLabel*>())
    {
        if (label->text().contains(text))
            return true;
    }
    return false;
}

/// Checks what the dialog promises, then closes it without running anything.
void read_and_close(QWidget* dialog, driver_problem problem)
{
    REQUIRE(has_label(dialog, driver_problem_title(problem)));
    REQUIRE(has_label(dialog, QStringLiteral("one permission prompt")));
    REQUIRE(has_label(dialog, QStringLiteral("Nothing is downloaded")));
    require_usable(button(dialog, QStringLiteral("Copy details")),
                   "Copy details");
    REQUIRE(button(dialog, set_up_text()) != nullptr);
    REQUIRE_EQ(button(dialog, set_up_text())->accessibleName(),
               set_up_text());
    click(button(dialog, QStringLiteral("Close")));
}

} // namespace

TEST_CASE("setup: the first screen names the missing piece and offers the set-up")
{
    for (const piece& each : kPieces)
    {
        Session s(std::string("setup: the first screen: ") + each.name);
        s.step(QString::fromLatin1("a PC where %1").arg(each.name));
        tell(s, each.problem);
        s.step(QStringLiteral("start the program"));
        s.restart_window();

        s.step(QStringLiteral("read the first screen"));
        NoteBanner* note = note_with_button(
            s.simple_page(), set_up_text());
        REQUIRE(note != nullptr);
        REQUIRE_EQ(note->text(), driver_problem_body(each.problem));
        REQUIRE(note->get_kind() == NoteBanner::kind::warn);

        s.step(QStringLiteral("choose Set up Virtual Serial Studio"));
        const driver_problem problem = each.problem;
        s.pilot().expect_type<support::DriverSetupDialog>(
            "the set-up dialog",
            [problem](QWidget* dialog) { read_and_close(dialog, problem); });
        click(note->action_button());
        s.settle();
        s.pilot().check();
        // Nothing was made or changed by looking.
        st::require_counts(s, {0, 0});
        s.finish();
    }
}

TEST_CASE("setup: a port that cannot be made says which piece is missing")
{
    for (const piece& each : kPieces)
    {
        Session s(std::string("setup: creating a port: ") + each.name);
        tell(s, each.problem);
        s.step(QStringLiteral("choose a job and press Create"));
        auto* form = st::open_job(s, job::pair);
        // The engine's own answer is one of the bare codes.
        s.fake().fail_next(vss::make_error_code(vss::errc::permission_denied));
        click(form->create_button());
        s.settle();

        s.step(QStringLiteral("read the error"));
        NoteBanner* error = note_with_button(
            s.simple_page(), set_up_text());
        REQUIRE(error != nullptr);
        REQUIRE_EQ(error->text(), driver_problem_body(each.problem));
        REQUIRE(!error->text().contains(QStringLiteral("permission denied")));
        st::require_counts(s, {0, 0});

        s.step(QStringLiteral("choose Set up Virtual Serial Studio"));
        const driver_problem problem = each.problem;
        s.pilot().expect_type<support::DriverSetupDialog>(
            "the set-up dialog",
            [problem](QWidget* dialog) { read_and_close(dialog, problem); });
        click(error->action_button());
        s.settle();
        s.pilot().check();

        s.step(QStringLiteral("the PC is set up, so the next try works"));
        tell(s, driver_problem::none);
        st::create(s);
        st::require_counts(s, {2, 1});
        st::remove_shown_setup(s);
        s.finish();
    }
}

TEST_CASE("setup: a refusal with nothing wrong in the set-up is in words, with details")
{
    Session s("setup: a refusal with nothing wrong in the set-up");
    tell(s, driver_problem::none);
    s.step(QStringLiteral("choose a job and press Create"));
    auto* form = st::open_job(s, job::pair);
    s.fake().fail_next(vss::make_error_code(vss::errc::permission_denied));
    click(form->create_button());
    s.settle();

    s.step(QStringLiteral("read the error"));
    NoteBanner* error =
        note_with_button(s.simple_page(), QStringLiteral("Copy details"));
    REQUIRE(error != nullptr);
    // Never the bare code, and there is no set-up to offer.
    REQUIRE(error->text().contains(QStringLiteral("Setup check")));
    REQUIRE(!error->text().contains(QStringLiteral("permission denied")));
    REQUIRE(note_with_button(s.simple_page(), set_up_text()) ==
            nullptr);
    st::require_counts(s, {0, 0});

    s.step(QStringLiteral("choose Copy details"));
    click(error->action_button());
    REQUIRE(last_driver_details().contains(QStringLiteral("permission denied")));
    s.finish();
}

TEST_CASE("setup: a PC that is ready is not asked to set up")
{
    Session s("setup: a PC that is ready");
    tell(s, driver_problem::none);
    s.step(QStringLiteral("start the program"));
    s.restart_window();
    REQUIRE(note_with_button(s.simple_page(), set_up_text()) ==
            nullptr);
    s.finish();
}
