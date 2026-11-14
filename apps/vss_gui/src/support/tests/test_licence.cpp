//
// Copyright (c) 2026 Virtual Serial Studio contributors
//
// SPDX-License-Identifier: GPL-3.0-or-later
//
// The words of the licence screens and the Licence page, for every state the
// spec lists (docs/gui-design/docs/screens/09-licence-and-support.md). The
// backend is the fake one, which can report any licence.
//

#include "qt_wait.hpp"
#include "test_framework.hpp"

#include "backend/fake_studio_backend.hpp"
#include "support/licence_dialog.hpp"
#include "support/licence_text.hpp"
#include "support/tests/licence_samples.hpp"
#include "widgets/controls.hpp"
#include "widgets/dialogs.hpp"

#include <QApplication>
#include <QClipboard>
#include <QDragEnterEvent>
#include <QDropEvent>
#include <QGuiApplication>
#include <QKeyEvent>
#include <QLabel>
#include <QLocale>
#include <QMimeData>
#include <QPushButton>
#include <QTimer>
#include <QUrl>

#include <chrono>
#include <functional>

using namespace vss::gui;
namespace lic = vss::license;
namespace sample = vss::gui::test_licence;
using vss::gui::support::LicenceDialog;

namespace
{

/// Whole days before the grace period ends.
lic::status grace_with_days_left(int days)
{
    const lic::licence value = sample::annual();
    lic::context where;
    where.computer_name = "LAB-RIG-07";
    where.now = value.expires_at + std::chrono::hours{24 * value.grace_days} -
                std::chrono::hours{24 * days + 6};
    return lic::evaluate(value, nullptr, where);
}

capabilities with_licences()
{
    capabilities caps;
    caps.licences = true;
    return caps;
}

/// Runs `handler` on the dialog of type `Dialog` that a modal call opens. The
/// handler records what it saw and presses a button; it must not use
/// REQUIRE, which cannot unwind through the event loop. If no dialog appears
/// the modal widget is rejected, so the test fails instead of hanging.
template <class Dialog>
void when_shown(const std::function<void(Dialog&)>& handler, int tries = 300)
{
    QTimer::singleShot(
        10, QApplication::instance(),
        [handler, tries]
        {
            for (QWidget* widget : QApplication::topLevelWidgets())
            {
                auto* dialog = qobject_cast<Dialog*>(widget);
                if (dialog != nullptr && dialog->isVisible())
                {
                    handler(*dialog);
                    return;
                }
            }
            if (tries > 0)
            {
                when_shown<Dialog>(handler, tries - 1);
                return;
            }
            if (QWidget* modal = QApplication::activeModalWidget())
                modal->close();
        });
}

struct error_seen
{
    int count = 0;
    QString title;
    QStringList texts;
    QStringList buttons;
    QString copied;
};

QPushButton* button_named(QWidget& parent, const QString& text)
{
    for (QPushButton* button : parent.findChildren<QPushButton*>())
    {
        if (button->text() == text)
            return button;
    }
    return nullptr;
}

/// Records the next "Could not import the licence" dialog, copies its details
/// when asked, then presses `press`.
void answer_error(error_seen& seen, const QString& press, bool copy_first,
                  const std::function<void()>& then = {})
{
    when_shown<ErrorDialog>(
        [&seen, press, copy_first, then](ErrorDialog& dialog)
        {
            ++seen.count;
            seen.title = dialog.windowTitle();
            seen.texts.clear();
            for (QLabel* label : dialog.findChildren<QLabel*>())
                seen.texts << label->text();
            seen.buttons.clear();
            for (QPushButton* button : dialog.findChildren<QPushButton*>())
                seen.buttons << button->text();
            if (copy_first)
            {
                if (QPushButton* copy =
                        button_named(dialog, QStringLiteral("Copy details")))
                {
                    QGuiApplication::clipboard()->clear();
                    copy->click();
                    seen.copied = QGuiApplication::clipboard()->text();
                }
            }
            if (QPushButton* target = button_named(dialog, press))
                target->click();
            if (then)
                then();
        });
}

struct confirm_seen
{
    int count = 0;
    QString title;
    QString body;
    QStringList buttons;
    QString default_button;
};

/// Records the next confirmation and presses the choice at `index`.
void answer_confirm(confirm_seen& seen, int index)
{
    when_shown<ConfirmDialog>(
        [&seen, index](ConfirmDialog& dialog)
        {
            ++seen.count;
            seen.title = dialog.windowTitle();
            seen.body.clear();
            for (QLabel* label : dialog.findChildren<QLabel*>())
                seen.body += label->text() + QLatin1Char('\n');
            seen.buttons.clear();
            for (int choice = 0; choice < dialog.choice_count(); ++choice)
            {
                seen.buttons << dialog.choice_button(choice)->text();
                if (dialog.choice_button(choice)->isDefault())
                    seen.default_button = dialog.choice_button(choice)->text();
            }
            dialog.choice_button(index)->click();
        });
}

QStringList toast_texts(QWidget& parent)
{
    QStringList texts;
    for (Toast* toast : parent.findChildren<Toast*>())
    {
        for (QLabel* label : toast->findChildren<QLabel*>())
            texts << label->text();
    }
    return texts;
}

QLabel* label_named(QWidget& parent, const char* name)
{
    return parent.findChild<QLabel*>(QString::fromLatin1(name));
}

QString role_of(const QPushButton* button)
{
    return button->property("role").toString();
}

licence_import_result imported(const lic::status& status)
{
    licence_import_result result;
    result.outcome = licence_import_outcome::imported;
    result.status = status;
    return result;
}

licence_import_result refused(lic::reason reason, const QString& detail)
{
    licence_import_result result;
    result.outcome = licence_import_outcome::not_valid;
    result.status = sample::unreadable_status(reason);
    result.detail = detail;
    return result;
}

} // namespace

// -- words
// ----------------------------------------------------------------------

TEST_CASE("licence text: the edition chip for every state of the table")
{
    namespace s = vss::gui::support;
    using tone = s::licence_tone;

    const auto none = s::edition_chip(lic::no_licence());
    REQUIRE_EQ(none.text, QStringLiteral("Community"));
    REQUIRE(none.tone == tone::neutral);

    const auto valid = s::edition_chip(sample::valid_status());
    REQUIRE_EQ(valid.text, QStringLiteral("Pro"));
    REQUIRE(valid.tone == tone::ok);

    lic::licence enterprise = sample::annual();
    enterprise.edition = lic::edition::enterprise;
    const auto big =
        s::edition_chip(sample::judge(enterprise, "2027-03-01T12:00:00Z"));
    REQUIRE_EQ(big.text, QStringLiteral("Enterprise"));
    REQUIRE(big.tone == tone::ok);

    const auto ten =
        s::edition_chip(sample::judge_days_left(sample::annual(), 10));
    REQUIRE_EQ(ten.text, QStringLiteral("Pro, 10 days left"));
    REQUIRE(ten.tone == tone::warn);
    REQUIRE_EQ(
        s::edition_chip(sample::judge_days_left(sample::annual(), 1)).text,
        QStringLiteral("Pro, 1 day left"));
    REQUIRE_EQ(
        s::edition_chip(sample::judge_days_left(sample::annual(), 0)).text,
        QStringLiteral("Pro, ends today"));

    const auto grace = s::edition_chip(sample::grace_status());
    REQUIRE_EQ(grace.text, QStringLiteral("Pro, licence ended"));
    REQUIRE(grace.tone == tone::warn);

    // Ended, not started, wrong date and not valid are Community with the
    // warn dot.
    for (const lic::status& status :
         {sample::expired_status(), sample::not_yet_valid_status(),
          sample::clock_suspect_status(), sample::wrong_computer_status(),
          sample::unreadable_status(lic::reason::bad_signature)})
    {
        const auto chip = s::edition_chip(status);
        REQUIRE_EQ(chip.text, QStringLiteral("Community"));
        REQUIRE(chip.tone == tone::warn);
    }

    // A trial.
    REQUIRE_EQ(
        s::edition_chip(sample::judge(sample::trial(), "2027-03-05T12:00:00Z"))
            .text,
        QStringLiteral("Pro trial"));
    REQUIRE_EQ(
        s::edition_chip(sample::judge_days_left(sample::trial(), 2)).text,
        QStringLiteral("Pro trial, 2 days left"));

    REQUIRE_EQ(s::edition_chip_name(valid),
               QStringLiteral("Edition: Pro. Open the licence page."));
}

TEST_CASE("licence text: the state chip and sentence for every state")
{
    namespace s = vss::gui::support;
    using tone = s::licence_tone;
    const QString computer = QStringLiteral("LAB-RIG-07");
    const QString ends = sample::date("2027-11-13T00:00:00Z");

    const auto check = [&](const lic::status& status, const QString& chip_text,
                           tone chip_tone, const QString& sentence)
    {
        const auto chip = s::state_chip(status);
        REQUIRE_EQ(chip.text, chip_text);
        REQUIRE(chip.tone == chip_tone);
        REQUIRE_EQ(s::state_sentence(status, computer), sentence);
    };

    check(lic::no_licence(), QStringLiteral("Community"), tone::neutral,
          QStringLiteral(
              "No licence is installed. This is the Community edition: free "
              "software, complete for capturing, monitoring and sharing "
              "serial ports."));
    check(sample::valid_status(), QStringLiteral("Valid"), tone::ok,
          QStringLiteral("Valid until %1.").arg(ends));
    check(sample::judge_days_left(sample::annual(), 10),
          QStringLiteral("Ends soon"), tone::warn,
          QStringLiteral("Valid until %1. 10 days left.").arg(ends));
    check(sample::judge_days_left(sample::annual(), 1),
          QStringLiteral("Ends soon"), tone::warn,
          QStringLiteral("Valid until %1. 1 day left.").arg(ends));
    check(sample::judge_days_left(sample::annual(), 0),
          QStringLiteral("Ends soon"), tone::warn,
          QStringLiteral("Valid until %1. Ends today.").arg(ends));
    check(sample::grace_status(), QStringLiteral("Ended"), tone::warn,
          QStringLiteral("Ended on %1. Official plugins keep working until "
                         "%2, then switch off.")
              .arg(ends, sample::date("2027-12-11T00:00:00Z")));
    check(sample::expired_status(), QStringLiteral("Ended"), tone::warn,
          QStringLiteral("Ended on %1. Studio is running as the Community "
                         "edition. Your ports, setups and files are "
                         "unchanged.")
              .arg(ends));
    check(sample::not_yet_valid_status(), QStringLiteral("Not started"),
          tone::neutral_hollow,
          QStringLiteral("Starts on %1.")
              .arg(sample::date("2026-11-13T00:00:00Z")));
    check(sample::clock_suspect_status(), QStringLiteral("Check the date"),
          tone::warn,
          QStringLiteral(
              "The date on this computer looks wrong. Official plugins are off "
              "until the date is right. If the date is right, create a "
              "support bundle and contact support."));

    const QString not_a_licence = QStringLiteral(
        "This file is not a valid Virtual Serial Studio licence. It may be "
        "damaged or incomplete. Ask for the file again.");
    check(sample::unreadable_status(lic::reason::bad_signature),
          QStringLiteral("Not valid"), tone::danger, not_a_licence);
    check(sample::unreadable_status(lic::reason::malformed),
          QStringLiteral("Not valid"), tone::danger, not_a_licence);
    check(sample::unreadable_status(lic::reason::newer_version),
          QStringLiteral("Not valid"), tone::danger,
          QStringLiteral("This licence needs a newer version of Studio. "
                         "Install the latest version, then import the licence "
                         "again."));
    check(sample::unreadable_status(lic::reason::revoked),
          QStringLiteral("Not valid"), tone::danger,
          QStringLiteral("This licence was withdrawn. Contact support."));
    check(sample::wrong_computer_status(), QStringLiteral("Not valid"),
          tone::danger,
          QStringLiteral("This licence is for other computers. This computer "
                         "is LAB-RIG-07. Ask your administrator for a licence "
                         "that names it."));
}

TEST_CASE("licence text: the summary rows and the notes under the card")
{
    namespace s = vss::gui::support;
    const QString ends = sample::date("2027-11-13T00:00:00Z");

    REQUIRE_EQ(s::edition_value(lic::no_licence()),
               QStringLiteral("Community"));
    REQUIRE_EQ(s::edition_value(sample::valid_status()), QStringLiteral("Pro"));
    REQUIRE_EQ(s::edition_value(
                   sample::judge(sample::trial(), "2027-03-05T12:00:00Z")),
               QStringLiteral("Pro (trial)"));
    // A licence that is not in force is the Community edition.
    REQUIRE_EQ(s::edition_value(sample::expired_status()),
               QStringLiteral("Community"));

    REQUIRE_EQ(s::seats_text(3), QStringLiteral("3 seats"));
    REQUIRE_EQ(s::seats_text(1), QStringLiteral("1 seat"));
    REQUIRE_EQ(s::seats_text(0), QStringLiteral("Site licence"));
    REQUIRE_EQ(s::term_text(sample::annual()),
               QStringLiteral("%1 to %2")
                   .arg(sample::date("2026-11-13T00:00:00Z"), ends));

    // Valid and not a trial: no note. Not started and none: no note.
    REQUIRE(!s::state_note(sample::valid_status()));
    REQUIRE(!s::state_note(lic::no_licence()));
    REQUIRE(!s::state_note(sample::not_yet_valid_status()));
    REQUIRE(!s::state_note(sample::clock_suspect_status()));

    const auto trial =
        s::state_note(sample::judge(sample::trial(), "2027-03-05T12:00:00Z"));
    REQUIRE(trial.has_value());
    REQUIRE(trial->tone == s::licence_note_tone::info);
    REQUIRE(!trial->renew);
    REQUIRE_EQ(trial->text,
               QStringLiteral("This is a 14 day trial of the Pro edition. It "
                              "ends on %1. Nothing you make during the trial "
                              "is lost.")
                   .arg(sample::date("2027-03-15T00:00:00Z")));

    const auto soon =
        s::state_note(sample::judge_days_left(sample::annual(), 5));
    REQUIRE(soon.has_value());
    REQUIRE(soon->tone == s::licence_note_tone::warn);
    REQUIRE(soon->renew);
    REQUIRE_EQ(soon->text,
               QStringLiteral("Your licence ends on %1. Renew to keep "
                              "official plugins and support.")
                   .arg(ends));

    const auto grace = s::state_note(sample::grace_status());
    REQUIRE(grace.has_value());
    REQUIRE(grace->tone == s::licence_note_tone::warn);
    REQUIRE(grace->renew);
    REQUIRE_EQ(grace->text,
               QStringLiteral("Your licence ended on %1. Official plugins "
                              "switch off on %2.")
                   .arg(ends, sample::date("2027-12-11T00:00:00Z")));

    const auto expired = s::state_note(sample::expired_status());
    REQUIRE(expired.has_value());
    REQUIRE(expired->tone == s::licence_note_tone::info);
    REQUIRE(expired->renew);
    REQUIRE_EQ(expired->text,
               QStringLiteral("Official plugins are off. Nothing you made is "
                              "lost: every port, setup and file still opens "
                              "and saves."));
}

TEST_CASE("licence text: every reason has its sentence")
{
    namespace s = vss::gui::support;
    const lic::status expired = sample::expired_status();
    const lic::status future = sample::not_yet_valid_status();

    REQUIRE_EQ(s::reason_sentence(lic::reason::ok, expired), QString());
    REQUIRE_EQ(s::reason_sentence(lic::reason::no_licence, lic::no_licence()),
               QStringLiteral("Needs a Pro licence."));
    REQUIRE_EQ(
        s::reason_sentence(lic::reason::not_in_licence, sample::valid_status()),
        QStringLiteral("Your licence does not include this."));
    REQUIRE_EQ(s::reason_sentence(lic::reason::expired, expired),
               QStringLiteral("Your licence ended on %1.")
                   .arg(sample::date("2027-11-13T00:00:00Z")));
    REQUIRE_EQ(s::reason_sentence(lic::reason::not_yet_valid, future),
               QStringLiteral("Your licence starts on %1.")
                   .arg(sample::date("2026-11-13T00:00:00Z")));
    REQUIRE_EQ(s::reason_sentence(lic::reason::clock_suspect,
                                  sample::clock_suspect_status()),
               QStringLiteral("The date on this computer looks wrong."));
    REQUIRE_EQ(s::reason_sentence(lic::reason::wrong_computer,
                                  sample::wrong_computer_status()),
               QStringLiteral("Your licence is for other computers."));
    // Damaged, newer, withdrawn and badly signed licences all read the same.
    for (const lic::reason reason :
         {lic::reason::malformed, lic::reason::newer_version,
          lic::reason::bad_signature, lic::reason::revoked})
    {
        REQUIRE_EQ(s::reason_sentence(reason, lic::no_licence()),
                   QStringLiteral("The installed licence is not valid."));
    }
}

TEST_CASE("licence text: the notices of the main window")
{
    namespace s = vss::gui::support;
    using kind = s::notice_kind;
    const QString ends = sample::date("2027-11-13T00:00:00Z");

    // Nothing for the states that need no notice.
    REQUIRE(!s::notice_for(lic::no_licence()));
    REQUIRE(!s::notice_for(sample::valid_status()));
    REQUIRE(!s::notice_for(sample::not_yet_valid_status()));
    REQUIRE(!s::notice_for(sample::wrong_computer_status()));

    const auto soon =
        s::notice_for(sample::judge_days_left(sample::annual(), 12));
    REQUIRE(soon.has_value());
    REQUIRE(soon->kind == kind::expiring);
    REQUIRE(soon->tone == s::licence_note_tone::info);
    REQUIRE_EQ(soon->text,
               QStringLiteral("Your licence ends on %1.").arg(ends));
    REQUIRE_EQ(s::notice_second_action(kind::expiring),
               QStringLiteral("Dismiss"));

    const auto grace = s::notice_for(grace_with_days_left(21));
    REQUIRE(grace.has_value());
    REQUIRE(grace->kind == kind::grace);
    REQUIRE(grace->tone == s::licence_note_tone::warn);
    REQUIRE_EQ(grace->text,
               QStringLiteral("Your licence ended on %1. Official plugins "
                              "switch off in 21 days.")
                   .arg(ends));
    const auto grace_tomorrow = s::notice_for(grace_with_days_left(1));
    REQUIRE(grace_tomorrow.has_value());
    REQUIRE_EQ(grace_tomorrow->text,
               QStringLiteral("Your licence ended on %1. Official plugins "
                              "switch off tomorrow.")
                   .arg(ends));
    const auto grace_today = s::notice_for(grace_with_days_left(0));
    REQUIRE(grace_today.has_value());
    REQUIRE_EQ(grace_today->text,
               QStringLiteral("Your licence ended on %1. Official plugins "
                              "switch off today.")
                   .arg(ends));
    REQUIRE_EQ(s::notice_second_action(kind::grace),
               QStringLiteral("Remind me tomorrow"));

    const auto expired = s::notice_for(sample::expired_status());
    REQUIRE(expired.has_value());
    REQUIRE(expired->kind == kind::expired);
    REQUIRE(expired->tone == s::licence_note_tone::info);
    REQUIRE_EQ(expired->text,
               QStringLiteral("Your licence has ended, so official plugins "
                              "are off. Studio is now the Community edition. "
                              "Your ports, setups and files are unchanged."));

    const auto clock = s::notice_for(sample::clock_suspect_status());
    REQUIRE(clock.has_value());
    REQUIRE(clock->kind == kind::clock_suspect);
    REQUIRE(clock->tone == s::licence_note_tone::warn);
    REQUIRE_EQ(clock->text,
               QStringLiteral("The date on this computer looks wrong, so "
                              "official plugins are off."));
    REQUIRE_EQ(s::notice_second_action(kind::clock_suspect),
               QStringLiteral("Dismiss"));
}

TEST_CASE("licence text: the support address is one reserved name")
{
    const QString address = vss::gui::support::support_address();
    REQUIRE(address.endsWith(QStringLiteral(".invalid")));
    REQUIRE(address.contains(QLatin1Char('@')));
    const QString renew =
        vss::gui::support::renew_text(QStringLiteral("VSS-2026-000123"));
    REQUIRE_EQ(renew,
               QStringLiteral("A licence is renewed by installing a new "
                              "licence file. Write to %1 and quote your "
                              "licence number, VSS-2026-000123. Studio does "
                              "not open a web page or send anything itself.")
                   .arg(address));
}

TEST_CASE("licence text: dates follow the locale")
{
    const QLocale saved = QLocale();
    QLocale::setDefault(QLocale(QLocale::German, QLocale::Germany));
    const QString german =
        vss::gui::support::licence_date(sample::at("2027-11-13T12:00:00Z"));
    QLocale::setDefault(QLocale(QLocale::English, QLocale::UnitedStates));
    const QString english =
        vss::gui::support::licence_date(sample::at("2027-11-13T12:00:00Z"));
    QLocale::setDefault(saved);
    REQUIRE(german.contains(QStringLiteral("November")));
    REQUIRE(german.contains(QStringLiteral("2027")));
    REQUIRE_NE(german, english);
    REQUIRE(english.contains(QStringLiteral("November")));
    // The long form, without the name of the day.
    REQUIRE(!english.contains(QStringLiteral("Saturday")));
    REQUIRE(!german.contains(QStringLiteral("Samstag")));
    REQUIRE(!english.startsWith(QLatin1Char(',')));
    REQUIRE_EQ(english, english.trimmed());
}

// -- the dialog
// -------------------------------------------------------------------

TEST_CASE("licence dialog: every state of the tables, updated in place")
{
    FakeStudioBackend backend;
    backend.set_capabilities(with_licences());
    LicenceDialog dialog(backend);
    QLabel* sentence = dialog.status_sentence();
    QLabel* edition = dialog.edition_heading();
    namespace s = vss::gui::support;

    struct state_case
    {
        lic::status status;
        QString edition;
        QString chip;
        StatusChip::kind kind;
        bool note;
        NoteBanner::kind note_kind;
        bool import_primary;
    };
    const QString ends = sample::date("2027-11-13T00:00:00Z");
    const std::vector<state_case> cases = {
        {lic::no_licence(), "Community", "Community", StatusChip::kind::neutral,
         false, NoteBanner::kind::info, true},
        {sample::valid_status(), "Pro", "Valid", StatusChip::kind::healthy,
         false, NoteBanner::kind::info, false},
        {sample::judge(sample::trial(), "2027-03-05T12:00:00Z"), "Pro (trial)",
         "Valid", StatusChip::kind::healthy, true, NoteBanner::kind::info,
         false},
        {sample::judge_days_left(sample::annual(), 10), "Pro", "Ends soon",
         StatusChip::kind::waiting, true, NoteBanner::kind::warn, false},
        {sample::grace_status(), "Pro", "Ended", StatusChip::kind::waiting,
         true, NoteBanner::kind::warn, false},
        {sample::expired_status(), "Community", "Ended",
         StatusChip::kind::waiting, true, NoteBanner::kind::info, true},
        {sample::not_yet_valid_status(), "Community", "Not started",
         StatusChip::kind::closed, false, NoteBanner::kind::info, false},
        {sample::clock_suspect_status(), "Community", "Check the date",
         StatusChip::kind::waiting, false, NoteBanner::kind::info, false},
        {sample::unreadable_status(lic::reason::bad_signature), "Community",
         "Not valid", StatusChip::kind::failed, false, NoteBanner::kind::info,
         true},
        {sample::wrong_computer_status(), "Community", "Not valid",
         StatusChip::kind::failed, false, NoteBanner::kind::info, true},
    };
    for (const state_case& item : cases)
    {
        backend.set_licence(sample::view_of(item.status));
        // The same widgets are updated; nothing is rebuilt.
        REQUIRE(dialog.status_sentence() == sentence);
        REQUIRE_EQ(edition->text(), item.edition);
        REQUIRE_EQ(dialog.state_chip_widget()->text(), item.chip);
        REQUIRE(dialog.state_chip_widget()->get_kind() == item.kind);
        REQUIRE_EQ(
            sentence->text(),
            s::state_sentence(item.status, QStringLiteral("LAB-RIG-07")));
        REQUIRE_EQ(sentence->accessibleName(), sentence->text());
        REQUIRE_EQ(dialog.state_banner()->isVisibleTo(&dialog), item.note);
        if (item.note)
            REQUIRE(dialog.state_banner()->get_kind() == item.note_kind);
        REQUIRE_EQ(role_of(dialog.import_button()) == QLatin1String("primary"),
                   item.import_primary);
        REQUIRE_EQ(role_of(dialog.close_button()) == QLatin1String("primary"),
                   !item.import_primary);
    }

    // The note text and its action, for a licence that is ending.
    backend.set_licence(sample::view_of(sample::grace_status()));
    REQUIRE_EQ(dialog.state_banner()->text(),
               QStringLiteral("Your licence ended on %1. Official plugins "
                              "switch off on %2.")
                   .arg(ends, sample::date("2027-12-11T00:00:00Z")));
    REQUIRE(dialog.state_banner()->action_button() != nullptr);
    REQUIRE_EQ(dialog.state_banner()->action_button()->text(),
               QStringLiteral("How to renew"));
    REQUIRE(dialog.state_banner()->action_button()->isVisibleTo(&dialog));
    // The trial note has no action.
    backend.set_licence(sample::view_of(
        sample::judge(sample::trial(), "2027-03-05T12:00:00Z")));
    REQUIRE(!dialog.state_banner()->action_button()->isVisibleTo(&dialog));
}

TEST_CASE("licence dialog: the summary leaves out rows with no value")
{
    FakeStudioBackend backend;
    backend.set_capabilities(with_licences());
    LicenceDialog dialog(backend);
    const auto shown = [&dialog](const char* name)
    {
        QLabel* label = label_named(dialog, name);
        REQUIRE(label != nullptr);
        return label->isVisibleTo(&dialog);
    };

    // Community: an edition and a status, nothing else.
    REQUIRE(shown("licenceRowEdition"));
    REQUIRE(shown("licenceRowStatus"));
    REQUIRE(!shown("licenceRowLicensee"));
    REQUIRE(!shown("licenceRowNumber"));
    REQUIRE(!shown("licenceRowTerm"));
    REQUIRE(!shown("licenceRowSeats"));
    REQUIRE(!shown("licenceRowComputers"));
    REQUIRE(!shown("licenceRowIncludes"));

    lic::licence value = sample::annual();
    value.machines = {"LAB-*", "RIG-01"};
    backend.set_licence(
        sample::view_of(sample::judge(value, "2027-03-01T12:00:00Z")));
    REQUIRE_EQ(label_named(dialog, "licenceRowLicensee")->text(),
               QStringLiteral("Example Lab Pty Ltd"));
    REQUIRE(shown("licenceRowLicensee"));
    REQUIRE_EQ(label_named(dialog, "licenceRowNumber")->text(),
               QStringLiteral("VSS-2026-000123"));
    REQUIRE_EQ(label_named(dialog, "licenceRowTerm")->text(),
               vss::gui::support::term_text(value));
    REQUIRE_EQ(label_named(dialog, "licenceRowSeats")->text(),
               QStringLiteral("3 seats"));
    REQUIRE_EQ(label_named(dialog, "licenceRowSeatsNote")->text(),
               QStringLiteral("A seat is two computers: the one you work at "
                              "and one other that you control or monitor "
                              "from it. The serial device can be on either."));
    REQUIRE_EQ(label_named(dialog, "licenceRowComputers")->text(),
               QStringLiteral("LAB-*\nRIG-01"));
    REQUIRE_EQ(label_named(dialog, "licenceRowThisComputer")->text(),
               QStringLiteral("This computer is LAB-RIG-07."));
    REQUIRE_EQ(label_named(dialog, "licenceRowIncludes")->text(),
               QStringLiteral("Official plugins\nMicrosoft-signed driver\n"
                              "Support"));

    // A site licence, and a licence that does not name computers.
    lic::licence site = sample::annual();
    site.seats = 0;
    backend.set_licence(
        sample::view_of(sample::judge(site, "2027-03-01T12:00:00Z")));
    REQUIRE_EQ(label_named(dialog, "licenceRowSeats")->text(),
               QStringLiteral("Site licence"));
    REQUIRE(!label_named(dialog, "licenceRowSeatsNote")->isVisibleTo(&dialog));
    REQUIRE(!shown("licenceRowComputers"));
    lic::licence single = sample::annual();
    single.seats = 1;
    backend.set_licence(
        sample::view_of(sample::judge(single, "2027-03-01T12:00:00Z")));
    REQUIRE_EQ(label_named(dialog, "licenceRowSeats")->text(),
               QStringLiteral("1 seat"));

    // A licence for other computers is still read, so the page shows whose it
    // is and which computers it names.
    backend.set_licence(sample::view_of(sample::wrong_computer_status()));
    REQUIRE(shown("licenceRowLicensee"));
    REQUIRE(shown("licenceRowNumber"));
    REQUIRE(shown("licenceRowTerm"));
    REQUIRE_EQ(label_named(dialog, "licenceRowComputers")->text(),
               QStringLiteral("STUDIO-*"));
    REQUIRE_EQ(label_named(dialog, "licenceRowThisComputer")->text(),
               QStringLiteral("This computer is LAB-RIG-07."));
    backend.set_licence(
        sample::view_of(sample::judge(single, "2027-03-01T12:00:00Z")));

    // The licence number has a copy button that says what it copies.
    auto* copy =
        dialog.findChild<CopyButton*>(QStringLiteral("licenceCopyNumber"));
    REQUIRE(copy != nullptr);
    REQUIRE_EQ(copy->copy_text(), QStringLiteral("VSS-2026-000123"));
    REQUIRE_EQ(copy->accessibleName(), QStringLiteral("Copy licence number"));
    copy->click();
    REQUIRE_EQ(QGuiApplication::clipboard()->text(),
               QStringLiteral("VSS-2026-000123"));
    REQUIRE(toast_texts(dialog).contains(
        QStringLiteral("VSS-2026-000123 copied.")));
}

TEST_CASE("licence dialog: what this edition includes, and why not")
{
    FakeStudioBackend backend;
    backend.set_capabilities(with_licences());
    LicenceDialog dialog(backend);
    const QStringList features = {"plugins.official", "driver.signed",
                                  "support"};
    const auto chip_of = [&](const QString& feature)
    { return label_named(dialog, ("featureChip." + feature).toLatin1()); };
    const auto why_of = [&](const QString& feature)
    { return label_named(dialog, ("featureWhy." + feature).toLatin1()); };
    REQUIRE_EQ(label_named(dialog, "featureName.plugins.official")->text(),
               QStringLiteral("Official plugins"));
    REQUIRE_EQ(label_named(dialog, "featureName.driver.signed")->text(),
               QStringLiteral("Microsoft-signed driver"));
    REQUIRE_EQ(label_named(dialog, "featureName.support")->text(),
               QStringLiteral("Support"));

    const auto all_not_available = [&](const QString& why)
    {
        for (const QString& feature : features)
        {
            REQUIRE_EQ(chip_of(feature)->text(),
                       QStringLiteral("Not available"));
            REQUIRE_EQ(why_of(feature)->text(), why);
        }
    };

    // The reason code of the table, one by one.
    all_not_available(QStringLiteral("Needs a Pro licence."));
    backend.set_licence(sample::view_of(sample::expired_status()));
    all_not_available(QStringLiteral("Your licence ended on %1.")
                          .arg(sample::date("2027-11-13T00:00:00Z")));
    backend.set_licence(sample::view_of(sample::not_yet_valid_status()));
    all_not_available(QStringLiteral("Your licence starts on %1.")
                          .arg(sample::date("2026-11-13T00:00:00Z")));
    backend.set_licence(sample::view_of(sample::clock_suspect_status()));
    all_not_available(QStringLiteral("The date on this computer looks wrong."));
    backend.set_licence(
        sample::view_of(sample::unreadable_status(lic::reason::bad_signature)));
    all_not_available(QStringLiteral("The installed licence is not valid."));
    backend.set_licence(sample::view_of(sample::wrong_computer_status()));
    all_not_available(QStringLiteral("Your licence is for other computers."));

    // A licence in force: available, with no reason, or "does not include".
    lic::licence partial = sample::annual();
    partial.features = {"plugins.official"};
    backend.set_licence(
        sample::view_of(sample::judge(partial, "2027-03-01T12:00:00Z")));
    REQUIRE_EQ(chip_of("plugins.official")->text(),
               QStringLiteral("Available"));
    REQUIRE_EQ(why_of("plugins.official")->text(), QString());
    REQUIRE_EQ(chip_of("driver.signed")->text(),
               QStringLiteral("Not available"));
    REQUIRE_EQ(why_of("driver.signed")->text(),
               QStringLiteral("Your licence does not include this."));
    // The ended licence is still in force during the grace period.
    backend.set_licence(sample::view_of(sample::grace_status()));
    REQUIRE_EQ(chip_of("support")->text(), QStringLiteral("Available"));
}

TEST_CASE("licence dialog: other licences, and the administrator's licence")
{
    FakeStudioBackend backend;
    backend.set_capabilities(with_licences());
    LicenceDialog dialog(backend);

    // One file: no list of others.
    backend.set_licence(sample::view_of(sample::valid_status()));
    REQUIRE(!dialog.other_licences()->isVisibleTo(&dialog));

    licence_view view = sample::view_of(sample::valid_status());
    licence_file_view mine;
    mine.status = sample::expired_status();
    mine.file_name = QStringLiteral("old.vsslic");
    licence_file_view theirs;
    theirs.status = sample::grace_status();
    theirs.file_name = QStringLiteral("site.vsslic");
    theirs.machine_wide = true;
    view.files.push_back(mine);
    view.files.push_back(theirs);
    backend.set_licence(view);
    QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
    REQUIRE(dialog.other_licences()->isVisibleTo(&dialog));

    // The disclosure counts the other files.
    bool titled = false;
    for (QAbstractButton* button :
         dialog.other_licences()->findChildren<QAbstractButton*>())
        titled =
            titled || button->text() ==
                          QStringLiteral("Other licences on this computer (2)");
    REQUIRE(titled);

    // Only the user's own file has a Remove button; the administrator's has a
    // chip instead.
    REQUIRE_EQ(
        dialog.findChildren<QPushButton*>(QStringLiteral("licenceRemoveOther"))
            .size(),
        1);
    bool admin_chip = false;
    for (QLabel* label :
         dialog.findChildren<QLabel*>(QStringLiteral("licenceAdminChip")))
        admin_chip =
            admin_chip ||
            label->text() == QStringLiteral("Installed by an administrator");
    REQUIRE(admin_chip);

    // The muted line.
    bool muted = false;
    for (QLabel* label : dialog.other_licences()->findChildren<QLabel*>())
        muted =
            muted || label->text() ==
                         QStringLiteral("Studio uses the licence that gives "
                                        "you the most.");
    REQUIRE(muted);

    // Removing the user's other file asks first and names the file.
    confirm_seen seen;
    answer_confirm(seen, 1);
    QPushButton* remove =
        dialog.findChild<QPushButton*>(QStringLiteral("licenceRemoveOther"));
    REQUIRE(remove != nullptr);
    remove->click();
    REQUIRE_EQ(seen.count, 1);
    REQUIRE_EQ(backend.removed_licences(),
               QStringList{QStringLiteral("old.vsslic")});
}

TEST_CASE(
    "licence dialog: Import is disabled when this build keeps no licences")
{
    FakeStudioBackend backend;
    LicenceDialog dialog(backend);
    REQUIRE(!dialog.import_button()->isEnabled());
    REQUIRE_EQ(dialog.import_button()->toolTip(),
               QStringLiteral("Not available in this build"));
    REQUIRE_EQ(dialog.import_button()->accessibleDescription(),
               QStringLiteral("Not available in this build"));
    // The page still says it is the Community edition.
    REQUIRE_EQ(dialog.edition_heading()->text(), QStringLiteral("Community"));

    // Nothing is imported, even when asked.
    int picked = 0;
    dialog.set_file_picker(
        [&picked]
        {
            ++picked;
            return QStringLiteral("a.vsslic");
        });
    dialog.choose_and_import();
    REQUIRE_EQ(picked, 0);
    REQUIRE(backend.imported_paths().isEmpty());

    // With licences, Import is on and has no tooltip.
    backend.set_capabilities(with_licences());
    LicenceDialog enabled(backend);
    REQUIRE(enabled.import_button()->isEnabled());
    REQUIRE_EQ(enabled.import_button()->toolTip(), QString());
}

TEST_CASE(
    "licence dialog: initial focus is on Close and the controls take focus")
{
    FakeStudioBackend backend;
    backend.set_capabilities(with_licences());
    backend.set_licence(sample::view_of(sample::valid_status()));
    LicenceDialog dialog(backend);
    REQUIRE(dialog.close_button()->isDefault());
    for (QPushButton* button : {dialog.import_button(), dialog.close_button(),
                                dialog.remove_button()})
    {
        REQUIRE((button->focusPolicy() & Qt::TabFocus) != 0);
        REQUIRE(!button->accessibleName().isEmpty() ||
                !button->text().isEmpty());
        REQUIRE(button->minimumSizeHint().height() >= 24 ||
                button->sizeHint().height() >= 24);
    }
}

// -- importing
// ---------------------------------------------------------------------

TEST_CASE("licence import: a valid licence updates the page in place")
{
    FakeStudioBackend backend;
    backend.set_capabilities(with_licences());
    LicenceDialog dialog(backend);
    QString picked;
    dialog.set_file_picker(
        [&picked]
        {
            picked = QStringLiteral("C:/inbox/from-mail.vsslic");
            return picked;
        });
    const lic::status valid = sample::valid_status();
    backend.set_import_result(imported(valid), sample::view_of(valid));

    QLabel* sentence = dialog.status_sentence();
    REQUIRE_EQ(dialog.state_chip_widget()->text(), QStringLiteral("Community"));
    dialog.import_button()->click();

    REQUIRE_EQ(picked, QStringLiteral("C:/inbox/from-mail.vsslic"));
    REQUIRE_EQ(backend.imported_paths(), QStringList{picked});
    // The same dialog, the same widgets, the new licence.
    REQUIRE(dialog.status_sentence() == sentence);
    REQUIRE_EQ(dialog.state_chip_widget()->text(), QStringLiteral("Valid"));
    REQUIRE_EQ(sentence->text(),
               QStringLiteral("Valid until %1.")
                   .arg(sample::date("2027-11-13T00:00:00Z")));
    REQUIRE_EQ(dialog.edition_heading()->text(), QStringLiteral("Pro"));
    REQUIRE(dialog.remove_button()->isVisibleTo(&dialog));
    REQUIRE(!dialog.import_banner()->isVisibleTo(&dialog));
    REQUIRE(toast_texts(dialog).contains(
        QStringLiteral("Licence imported. This is now the Pro edition.")));

    // A picker that returns nothing imports nothing.
    dialog.set_file_picker([] { return QString(); });
    dialog.import_button()->click();
    REQUIRE_EQ(backend.imported_paths().size(), 1);
}

TEST_CASE("licence import: the keyboard alone can import")
{
    FakeStudioBackend backend;
    backend.set_capabilities(with_licences());
    LicenceDialog dialog(backend);
    int picked = 0;
    dialog.set_file_picker(
        [&picked]
        {
            ++picked;
            return QString();
        });
    // Space on the focused button presses it.
    dialog.import_button()->setFocus();
    QKeyEvent press(QEvent::KeyPress, Qt::Key_Space, Qt::NoModifier,
                    QStringLiteral(" "));
    QKeyEvent release(QEvent::KeyRelease, Qt::Key_Space, Qt::NoModifier,
                      QStringLiteral(" "));
    QApplication::sendEvent(dialog.import_button(), &press);
    QApplication::sendEvent(dialog.import_button(), &release);
    REQUIRE_EQ(picked, 1);
}

TEST_CASE("licence import: an ended or future licence is imported with a note")
{
    FakeStudioBackend backend;
    backend.set_capabilities(with_licences());
    LicenceDialog dialog(backend);
    dialog.set_file_picker([] { return QStringLiteral("x.vsslic"); });
    const QString ends = sample::date("2027-11-13T00:00:00Z");

    const auto import_and_check = [&](const lic::status& status,
                                      NoteBanner::kind kind,
                                      const QString& text)
    {
        backend.set_import_result(imported(status), sample::view_of(status));
        dialog.import_button()->click();
        REQUIRE(dialog.import_banner()->isVisibleTo(&dialog));
        REQUIRE(dialog.import_banner()->get_kind() == kind);
        REQUIRE_EQ(dialog.import_banner()->text(), text);
    };

    import_and_check(
        sample::expired_status(), NoteBanner::kind::warn,
        QStringLiteral("This licence ended on %1. It was imported, but it does "
                       "not turn anything on.")
            .arg(ends));
    import_and_check(
        sample::grace_status(), NoteBanner::kind::warn,
        QStringLiteral("This licence ended on %1. It was imported, but it does "
                       "not turn anything on.")
            .arg(ends));
    import_and_check(sample::not_yet_valid_status(), NoteBanner::kind::info,
                     QStringLiteral("This licence starts on %1.")
                         .arg(sample::date("2026-11-13T00:00:00Z")));

    // The banner describes the import. It goes when the licence changes for
    // another reason, such as a removal.
    backend.set_licence(sample::view_of(sample::valid_status()));
    REQUIRE(!dialog.import_banner()->isVisibleTo(&dialog));
}

TEST_CASE("licence import: the same licence again says so and changes nothing")
{
    FakeStudioBackend backend;
    backend.set_capabilities(with_licences());
    backend.set_licence(sample::view_of(sample::valid_status()));
    LicenceDialog dialog(backend);
    dialog.set_file_picker([] { return QStringLiteral("x.vsslic"); });
    licence_import_result result;
    result.outcome = licence_import_outcome::already_installed;
    backend.set_import_result(result);
    const QString before = dialog.status_sentence()->text();
    dialog.import_button()->click();
    REQUIRE(toast_texts(dialog).contains(
        QStringLiteral("That licence is already installed.")));
    REQUIRE_EQ(dialog.status_sentence()->text(), before);
    REQUIRE_EQ(dialog.state_chip_widget()->text(), QStringLiteral("Valid"));
}

TEST_CASE(
    "licence import: a file that is not valid copies nothing and explains")
{
    struct refusal
    {
        lic::reason reason;
        QString sentence;
        QString code;
    };
    const std::vector<refusal> refusals = {
        {lic::reason::bad_signature,
         QStringLiteral("This file is not a valid Virtual Serial Studio "
                        "licence. It may be damaged or incomplete. Ask for the "
                        "file again."),
         QStringLiteral("bad_signature")},
        {lic::reason::malformed,
         QStringLiteral("This file is not a valid Virtual Serial Studio "
                        "licence. It may be damaged or incomplete. Ask for the "
                        "file again."),
         QStringLiteral("malformed")},
        {lic::reason::newer_version,
         QStringLiteral("This licence needs a newer version of Studio. Install "
                        "the latest version, then import the licence again."),
         QStringLiteral("newer_version")},
        {lic::reason::revoked,
         QStringLiteral("This licence was withdrawn. Contact support."),
         QStringLiteral("revoked")},
        {lic::reason::wrong_computer,
         QStringLiteral("This licence is for other computers. This computer is "
                        "LAB-RIG-07. Ask your administrator for a licence that "
                        "names it."),
         QStringLiteral("wrong_computer")}};

    for (const refusal& item : refusals)
    {
        FakeStudioBackend backend;
        backend.set_capabilities(with_licences());
        // A valid licence is installed. An invalid file must not replace it.
        backend.set_licence(sample::view_of(sample::valid_status()));
        LicenceDialog dialog(backend);
        dialog.set_file_picker([]
                               { return QStringLiteral("C:/in/bad.vsslic"); });
        backend.set_import_result(
            refused(item.reason, QStringLiteral("the library's message")));

        const QString sentence_before = dialog.status_sentence()->text();
        const QString chip_before = dialog.state_chip_widget()->text();
        const QString edition_before = dialog.edition_heading()->text();

        error_seen seen;
        answer_error(seen, QStringLiteral("Close"), true);
        dialog.import_button()->click();

        REQUIRE_EQ(seen.count, 1);
        REQUIRE_EQ(seen.title, QStringLiteral("Could not import the licence"));
        REQUIRE(seen.texts.contains(item.sentence));
        REQUIRE(seen.buttons.contains(QStringLiteral("Copy details")));
        REQUIRE(seen.buttons.contains(QStringLiteral("Choose another file")));
        REQUIRE(seen.buttons.contains(QStringLiteral("Close")));
        // Copy details: the file name, the reason code and the library's
        // message.
        REQUIRE(seen.copied.contains(QStringLiteral("bad.vsslic")));
        REQUIRE(seen.copied.contains(item.code));
        REQUIRE(seen.copied.contains(QStringLiteral("the library's message")));

        // Nothing the page shows changed.
        REQUIRE_EQ(dialog.status_sentence()->text(), sentence_before);
        REQUIRE_EQ(dialog.state_chip_widget()->text(), chip_before);
        REQUIRE_EQ(dialog.edition_heading()->text(), edition_before);
        REQUIRE_EQ(edition_before, QStringLiteral("Pro"));
        REQUIRE(!dialog.import_banner()->isVisibleTo(&dialog));
        REQUIRE(toast_texts(dialog).isEmpty());
        REQUIRE_EQ(backend.licence().status.state, lic::state::valid);
    }
}

TEST_CASE("licence import: a file that cannot be read is named")
{
    FakeStudioBackend backend;
    backend.set_capabilities(with_licences());
    LicenceDialog dialog(backend);
    dialog.set_file_picker([] { return QStringLiteral("C:/in/gone.vsslic"); });
    licence_import_result result;
    result.outcome = licence_import_outcome::unreadable;
    backend.set_import_result(result);

    error_seen seen;
    answer_error(seen, QStringLiteral("Close"), true);
    dialog.import_button()->click();
    REQUIRE_EQ(seen.count, 1);
    REQUIRE_EQ(seen.title, QStringLiteral("Could not import the licence"));
    REQUIRE(seen.texts.contains(QStringLiteral(
        "Studio could not read gone.vsslic. Check that the file still exists "
        "and that you are allowed to open it.")));
    REQUIRE(seen.copied.contains(QStringLiteral("gone.vsslic")));
    REQUIRE_EQ(dialog.state_chip_widget()->text(), QStringLiteral("Community"));
}

TEST_CASE("licence import: Choose another file asks for a file again")
{
    FakeStudioBackend backend;
    backend.set_capabilities(with_licences());
    LicenceDialog dialog(backend);
    int picks = 0;
    dialog.set_file_picker(
        [&picks]
        {
            ++picks;
            return QStringLiteral("C:/in/%1.vsslic").arg(picks);
        });
    backend.set_import_result(
        refused(lic::reason::bad_signature, QStringLiteral("message")));

    error_seen first;
    error_seen second;
    // The first error asks for another file; the second is closed.
    answer_error(first, QStringLiteral("Choose another file"), false, [&second]
                 { answer_error(second, QStringLiteral("Close"), false); });
    dialog.import_button()->click();
    REQUIRE_EQ(first.count, 1);
    REQUIRE_EQ(second.count, 1);
    REQUIRE_EQ(picks, 2);
    REQUIRE_EQ(backend.imported_paths().size(), 2);
    REQUIRE_EQ(backend.imported_paths().last(),
               QStringLiteral("C:/in/2.vsslic"));
}

TEST_CASE("licence import: a .vsslic file dropped on the dialog is imported")
{
    FakeStudioBackend backend;
    backend.set_capabilities(with_licences());
    LicenceDialog dialog(backend);
    const lic::status valid = sample::valid_status();
    backend.set_import_result(imported(valid), sample::view_of(valid));

    QMimeData licence_file;
    licence_file.setUrls(
        {QUrl::fromLocalFile(QStringLiteral("C:/in/new.vsslic"))});
    QDragEnterEvent enter(QPointF(10, 10), Qt::CopyAction, &licence_file,
                          Qt::LeftButton, Qt::NoModifier);
    QApplication::sendEvent(&dialog, &enter);
    REQUIRE(enter.isAccepted());

    QDropEvent drop(QPointF(10, 10), Qt::CopyAction, &licence_file,
                    Qt::LeftButton, Qt::NoModifier);
    QApplication::sendEvent(&dialog, &drop);
    // The import runs after the drop has finished.
    REQUIRE(vss_test::wait_until(
        [&backend] { return backend.imported_paths().size() == 1; }));
    REQUIRE(backend.imported_paths().first().endsWith(
        QStringLiteral("new.vsslic")));
    REQUIRE_EQ(dialog.state_chip_widget()->text(), QStringLiteral("Valid"));

    // Another kind of file is not accepted and is not imported.
    QMimeData other;
    other.setUrls({QUrl::fromLocalFile(QStringLiteral("C:/in/notes.txt"))});
    QDragEnterEvent refused_enter(QPointF(10, 10), Qt::CopyAction, &other,
                                  Qt::LeftButton, Qt::NoModifier);
    refused_enter.ignore();
    QApplication::sendEvent(&dialog, &refused_enter);
    REQUIRE(!refused_enter.isAccepted());
    QDropEvent refused_drop(QPointF(10, 10), Qt::CopyAction, &other,
                            Qt::LeftButton, Qt::NoModifier);
    refused_drop.ignore();
    QApplication::sendEvent(&dialog, &refused_drop);
    QApplication::processEvents();
    REQUIRE_EQ(backend.imported_paths().size(), 1);
}

// -- removing
// ---------------------------------------------------------------------

TEST_CASE("licence remove: asks first, then removes the file by its name")
{
    FakeStudioBackend backend;
    backend.set_capabilities(with_licences());
    backend.set_licence(sample::view_of(sample::valid_status()));
    LicenceDialog dialog(backend);
    REQUIRE(dialog.remove_button()->isVisibleTo(&dialog));
    REQUIRE_EQ(dialog.remove_button()->text(),
               QStringLiteral("Remove licence"));

    // Cancel changes nothing, and Cancel is the default.
    confirm_seen cancelled;
    answer_confirm(cancelled, 0);
    dialog.remove_button()->click();
    REQUIRE_EQ(cancelled.count, 1);
    REQUIRE_EQ(cancelled.title, QStringLiteral("Remove this licence?"));
    REQUIRE(cancelled.body.contains(QStringLiteral(
        "Studio returns to the Community edition. Your ports, setups and files "
        "are unchanged. Keep the licence file so you can import it again.")));
    REQUIRE_EQ(cancelled.buttons,
               (QStringList{QStringLiteral("Cancel"),
                            QStringLiteral("Remove licence")}));
    REQUIRE_EQ(cancelled.default_button, QStringLiteral("Cancel"));
    REQUIRE(backend.removed_licences().isEmpty());
    REQUIRE_EQ(dialog.state_chip_widget()->text(), QStringLiteral("Valid"));

    // Remove licence removes the file and the page becomes Community.
    confirm_seen confirmed;
    answer_confirm(confirmed, 1);
    dialog.remove_button()->click();
    REQUIRE_EQ(confirmed.count, 1);
    REQUIRE_EQ(backend.removed_licences(),
               QStringList{QStringLiteral("VSS-2026-000123.vsslic")});
    REQUIRE_EQ(dialog.state_chip_widget()->text(), QStringLiteral("Community"));
    REQUIRE_EQ(dialog.edition_heading()->text(), QStringLiteral("Community"));
    REQUIRE(!dialog.remove_button()->isVisibleTo(&dialog));
}

TEST_CASE("licence remove: a machine-wide licence has no Remove button")
{
    FakeStudioBackend backend;
    backend.set_capabilities(with_licences());
    backend.set_licence(sample::view_of(sample::valid_status(), true,
                                        QStringLiteral("site.vsslic")));
    LicenceDialog dialog(backend);
    REQUIRE(!dialog.remove_button()->isVisibleTo(&dialog));

    // No licence at all: no Remove either.
    backend.set_licence({});
    REQUIRE(!dialog.remove_button()->isVisibleTo(&dialog));
    // A licence of the user's own brings it back.
    backend.set_licence(sample::view_of(sample::valid_status()));
    REQUIRE(dialog.remove_button()->isVisibleTo(&dialog));
}

// -- renewing
// -----------------------------------------------------------------------

TEST_CASE(
    "licence renew: the address and the number are copied, nothing is sent")
{
    FakeStudioBackend backend;
    backend.set_capabilities(with_licences());
    backend.set_licence(sample::view_of(sample::grace_status()));
    LicenceDialog dialog(backend);
    QPushButton* renew = dialog.state_banner()->action_button();
    REQUIRE(renew != nullptr);

    struct seen_renew
    {
        QString title;
        QStringList texts;
        QStringList buttons;
        QString number;
        QString address;
        QStringList toasts;
    } seen;
    when_shown<support::RenewDialog>(
        [&seen](support::RenewDialog& renew_dialog)
        {
            seen.title = renew_dialog.windowTitle();
            for (QLabel* label : renew_dialog.findChildren<QLabel*>())
                seen.texts << label->text();
            for (QPushButton* button :
                 renew_dialog.findChildren<QPushButton*>())
                seen.buttons << button->text();
            renew_dialog.copy_number_button()->click();
            seen.number = QGuiApplication::clipboard()->text();
            renew_dialog.copy_address_button()->click();
            seen.address = QGuiApplication::clipboard()->text();
            seen.toasts = toast_texts(renew_dialog);
            renew_dialog.close_button()->click();
        });
    renew->click();

    REQUIRE_EQ(seen.title, QStringLiteral("Renew your licence"));
    REQUIRE(seen.texts.contains(
        vss::gui::support::renew_text(QStringLiteral("VSS-2026-000123"))));
    REQUIRE(seen.buttons.contains(QStringLiteral("Copy licence number")));
    REQUIRE(seen.buttons.contains(QStringLiteral("Copy address")));
    REQUIRE(seen.buttons.contains(QStringLiteral("Close")));
    REQUIRE_EQ(seen.number, QStringLiteral("VSS-2026-000123"));
    REQUIRE_EQ(seen.address, vss::gui::support::support_address());
    REQUIRE(seen.toasts.contains(QStringLiteral("VSS-2026-000123 copied.")));
}
