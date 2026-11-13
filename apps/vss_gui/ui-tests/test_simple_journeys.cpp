//
// Copyright (c) 2026 Virtual Serial Studio contributors
//
// SPDX-License-Identifier: GPL-3.0-or-later
//
// Simple mode, job by job. Each journey starts at the window, clicks through
// the pages as a person does, checks the ports and links it made (in the
// backend, on the page and, through the View menu, in the Ports dock and on
// the canvas), and then removes the setup.
//

#include "simple_steps.hpp"

#include "dialogs/templates_dialog.hpp"
#include "simple/setup_page.hpp"
#include "widgets/notes.hpp"

#include <QComboBox>
#include <QPlainTextEdit>

namespace
{

using namespace vss::gui;
using namespace vss::gui::ui;
using vss::gui::simple::job;
namespace st = vss::gui::ui::steps;

/// The first visible "Close <port>" button of a setup page, or null.
QPushButton* first_close_button(QWidget* page)
{
    for (auto* candidate : page->findChildren<QPushButton*>())
    {
        if (candidate->isVisibleTo(page) &&
            candidate->accessibleName().startsWith(QStringLiteral("Close ")))
            return candidate;
    }
    return nullptr;
}

} // namespace

TEST_CASE("simple: give my program a serial port")
{
    Session s("simple: give my program a serial port");
    s.step(QStringLiteral("choose 'Give my program a serial port'"));
    auto* form = st::open_job(s, job::port);
    s.step(QStringLiteral("type the program name"));
    auto fields = st::program_fields(form);
    REQUIRE_EQ(fields.size(), 1);
    type_into(fields[0], QStringLiteral("PuTTY"));
    s.step(QStringLiteral("click Create"));
    auto* page = st::create(s);
    st::require_counts(s, {1, 0});
    REQUIRE(page->steps_text().contains(QStringLiteral("In PuTTY, open")));
    REQUIRE_EQ(s.simple_page()->list()->count(), 1);
    st::verify_in_advanced(s);
    s.step(QStringLiteral("remove the setup"));
    st::remove_shown_setup(s);
    s.finish();
}

TEST_CASE("simple: connect two programs to each other")
{
    Session s("simple: connect two programs to each other");
    s.step(QStringLiteral("choose 'Connect two programs to each other'"));
    auto* form = st::open_job(s, job::pair);
    auto fields = st::program_fields(form);
    REQUIRE_EQ(fields.size(), 2);
    type_into(fields[0], QStringLiteral("Terminal"));
    type_into(fields[1], QStringLiteral("Logger"));
    s.step(QStringLiteral("click Create"));
    auto* page = st::create(s);
    st::require_counts(s, {2, 1});
    REQUIRE(page->steps_text().contains(QStringLiteral("Terminal")));
    REQUIRE(page->steps_text().contains(QStringLiteral("Logger")));
    st::verify_in_advanced(s);
    s.step(QStringLiteral("remove the setup"));
    st::remove_shown_setup(s);
    s.finish();
}

TEST_CASE("simple: connect two programs when Windows keeps some COM numbers")
{
    // A PC with the Studio driver: ports are COM numbers.
    auto windows = full_capabilities();
    windows.com_numbers = true;
    Session s(
        "simple: connect two programs when Windows keeps some COM numbers",
        Session::mode::simple, windows);
    // COM3 to COM6 belong to ports that are no longer plugged in; COM7 is held
    // by something the list of numbers does not show.
    s.fake().set_taken_com_numbers({3, 4, 5, 6});
    s.fake().set_hidden_com_numbers({7});
    s.step(QStringLiteral("choose 'Connect two programs to each other'"));
    auto* form = st::open_job(s, job::pair);
    s.step(QStringLiteral("look at the numbers offered"));
    for (auto* combo :
         form->findChildren<QComboBox*>(QStringLiteral("jobFormPort")))
    {
        for (int index = 0; index < combo->count(); ++index)
        {
            // 0 is Automatic, which lists first.
            const unsigned number = combo->itemData(index).toUInt();
            REQUIRE(index == 0 ? number == 0 : number > 6);
        }
    }
    s.step(QStringLiteral("choose COM7 for the first program"));
    {
        auto* combo =
            form->findChildren<QComboBox*>(QStringLiteral("jobFormPort"))
                .value(0);
        REQUIRE(combo != nullptr);
        const int index = combo->findText(QStringLiteral("COM7"));
        REQUIRE(index >= 0);
        combo->setCurrentIndex(index);
        Q_EMIT combo->activated(index);
        s.settle();
    }
    s.step(QStringLiteral("click Create: Windows refuses COM7"));
    click(form->create_button());
    wait_for("the failed build", [&] { return !form->busy(); });
    s.settle();
    // The form stays open with the failure, and nothing was made.
    REQUIRE(s.simple_page()->form() == form);
    st::require_counts(s, {0, 0});
    bool named = false;
    for (auto* banner : form->findChildren<NoteBanner*>())
    {
        if (banner->isVisibleTo(form) &&
            banner->text().contains(QStringLiteral("COM7 already belongs")))
            named = true;
    }
    REQUIRE(named);
    s.step(QStringLiteral("click Create again with the new numbers"));
    auto* page = st::create(s);
    st::require_counts(s, {2, 1});
    REQUIRE(!page->steps_text().contains(QStringLiteral("COM7")));
    st::verify_in_advanced(s);
    s.step(QStringLiteral("remove the setup"));
    st::remove_shown_setup(s);
    s.finish();
}

TEST_CASE("simple: use my device through a new port")
{
    Session s("simple: use my device through a new port");
    st::plug_in_devices(s, {QStringLiteral("COM7")});
    s.step(QStringLiteral("choose 'Use my device through a new port'"));
    auto* form = st::open_job(s, job::attach);
    s.step(QStringLiteral("pick the device"));
    st::choose_device(form, QStringLiteral("COM7"));
    s.step(QStringLiteral("click Create"));
    st::create(s);
    st::require_counts(s, {2, 1});
    st::verify_in_advanced(s);
    s.step(QStringLiteral("remove the setup"));
    st::remove_shown_setup(s);
    s.finish();
}

TEST_CASE("simple: share my device with several programs")
{
    Session s("simple: share my device with several programs");
    st::plug_in_devices(s, {QStringLiteral("COM7")});
    s.step(QStringLiteral("choose 'Share my device with several programs'"));
    auto* form = st::open_job(s, job::share);
    st::choose_device(form, QStringLiteral("COM7"));
    s.step(QStringLiteral("name two programs"));
    auto fields = st::program_fields(form);
    REQUIRE_EQ(fields.size(), 2);
    type_into(fields[0], QStringLiteral("Tera Term"));
    type_into(fields[1], QStringLiteral("Logger"));
    s.step(QStringLiteral("add a third program"));
    click(button(form, QStringLiteral("Add a program")));
    REQUIRE_EQ(st::program_fields(form).size(), 3);
    s.step(QStringLiteral("click Create"));
    auto* page = st::create(s);
    st::require_counts(s, {4, 1});
    REQUIRE(page->steps_text().contains(QStringLiteral("Tera Term")));
    REQUIRE(page->steps_text().contains(QStringLiteral("Do not open")));
    st::verify_in_advanced(s);
    s.step(QStringLiteral("add a fourth program from the setup page"));
    click(button(page, QStringLiteral("Add a program")));
    s.settle();
    st::require_counts(s, {5, 1});
    s.step(QStringLiteral("remove the setup"));
    st::remove_shown_setup(s);
    s.finish();
}

TEST_CASE("simple: try my device from this window")
{
    Session s("simple: try my device from this window");
    st::plug_in_devices(s, {QStringLiteral("COM7")});
    s.step(QStringLiteral("choose 'Try my device from this window'"));
    auto* form = st::open_job(s, job::talk);
    st::choose_device(form, QStringLiteral("COM7"));
    s.step(QStringLiteral("click Create"));
    auto* page = st::create(s);
    st::require_counts(s, {1, 0});
    s.step(QStringLiteral("type to the device and press Enter"));
    auto* input = named<QLineEdit>(page, "setupInput");
    type_into(input, QStringLiteral("AT"));
    key(input, Qt::Key_Return);
    s.settle();
    REQUIRE_EQ(s.fake().sent().size(), 1);
    REQUIRE(s.fake().sent().front().startsWith("AT"));
    REQUIRE(input->text().isEmpty());
    s.step(QStringLiteral("remove the setup"));
    st::remove_shown_setup(s);
    s.finish();
}

TEST_CASE("simple: let other computers use my device")
{
    Session s("simple: let other computers use my device");
    st::plug_in_devices(s, {QStringLiteral("COM7")});
    s.step(QStringLiteral("choose 'Let other computers use my device'"));
    auto* form = st::open_job(s, job::network);
    st::choose_device(form, QStringLiteral("COM7"));
    s.step(QStringLiteral("click Create"));
    st::create(s);
    REQUIRE_EQ(s.backend().ports().size(), 2);
    st::verify_in_advanced(s);
    s.step(QStringLiteral("remove the setup"));
    st::remove_shown_setup(s);
    s.finish();
}

TEST_CASE("simple: put several on one line")
{
    Session s("simple: put several on one line");
    s.step(QStringLiteral("choose 'Put several on one line'"));
    auto* form = st::open_job(s, job::bus);
    REQUIRE_EQ(st::program_fields(form).size(), 2);
    s.step(QStringLiteral("add a third program"));
    click(button(form, QStringLiteral("Add a program")));
    REQUIRE_EQ(st::program_fields(form).size(), 3);
    s.step(QStringLiteral("click Create"));
    auto* page = st::create(s);
    st::require_counts(s, {3, 1});
    st::verify_in_advanced(s);
    const vss::link_id link_id = s.fake().links().front().id;
    s.step(QStringLiteral("add a fourth program from the setup page"));
    click(button(page, QStringLiteral("Add a program")));
    s.settle();
    st::require_counts(s, {4, 1});
    REQUIRE_EQ(s.fake().links().front().id, link_id);
    REQUIRE_EQ(s.fake().links().front().sources.size(), qsizetype{4});
    s.step(QStringLiteral("close the first program port"));
    auto* close = first_close_button(st::setup_page(s));
    REQUIRE(close != nullptr);
    click(close);
    s.settle();
    st::require_counts(s, {3, 1});
    REQUIRE_EQ(s.fake().links().front().id, link_id);
    REQUIRE_EQ(s.fake().links().front().sources.size(), qsizetype{3});
    s.step(QStringLiteral("remove the setup"));
    st::remove_shown_setup(s);
    s.finish();
}

TEST_CASE("simple: send several into one")
{
    Session s("simple: send several into one");
    s.step(QStringLiteral("choose 'Send several into one'"));
    st::open_job(s, job::combine);
    s.step(QStringLiteral("click Create"));
    auto* page = st::create(s);
    st::require_counts(s, {3, 1});
    st::verify_in_advanced(s);
    const vss::link_id link_id = s.fake().links().front().id;
    const auto receiver = s.fake().links().front().targets.front();
    s.step(QStringLiteral("add a sender from the setup page"));
    click(button(page, QStringLiteral("Add a program")));
    s.settle();
    st::require_counts(s, {4, 1});
    REQUIRE_EQ(s.fake().links().front().id, link_id);
    REQUIRE_EQ(s.fake().links().front().sources.size(), qsizetype{3});
    REQUIRE_EQ(s.fake().links().front().targets.front(), receiver);
    s.step(QStringLiteral("close the first sender"));
    auto* close = first_close_button(st::setup_page(s));
    REQUIRE(close != nullptr);
    click(close);
    s.settle();
    st::require_counts(s, {3, 1});
    REQUIRE_EQ(s.fake().links().front().id, link_id);
    REQUIRE_EQ(s.fake().links().front().sources.size(), qsizetype{2});
    REQUIRE_EQ(s.fake().links().front().targets.front(), receiver);
    s.step(QStringLiteral("remove the setup"));
    st::remove_shown_setup(s);
    s.finish();
}

TEST_CASE("simple: let programs take turns sending to a device")
{
    Session s("simple: let programs take turns sending to a device");
    st::plug_in_devices(s, {QStringLiteral("COM7")});
    s.step(QStringLiteral("choose 'Let programs take turns sending'"));
    auto* form = st::open_job(s, job::turns);
    st::choose_device(form, QStringLiteral("COM7"));
    s.step(QStringLiteral("click Create"));
    auto* page = st::create(s);
    st::require_counts(s, {3, 1});
    REQUIRE(page->steps_text().contains(QStringLiteral("Choose below")));
    s.step(QStringLiteral("one duplex link from the device, so replies come "
                          "back; the first program sends"));
    const auto link = s.fake().links().front();
    REQUIRE(link.mode == vss::link_mode::duplex);
    REQUIRE(link.options.policy == vss::merge_policy::owner);
    REQUIRE_EQ(link.sources.size(), qsizetype{1});
    REQUIRE_EQ(link.targets.size(), qsizetype{2});
    REQUIRE_EQ(link.owner, link.targets.front());
    st::verify_in_advanced(s);
    s.step(QStringLiteral("remove the setup"));
    st::remove_shown_setup(s);
    s.finish();
}

TEST_CASE("simple: change a setup")
{
    Session s("simple: change a setup");
    s.step(QStringLiteral("make a pair"));
    auto* form = st::open_job(s, job::pair);
    auto fields = st::program_fields(form);
    type_into(fields[0], QStringLiteral("Terminal"));
    auto* page = st::create(s);
    st::require_counts(s, {2, 1});
    s.step(QStringLiteral("click Change"));
    click(button(page, QStringLiteral("Change…")));
    wait_for("the form to edit",
             [&] { return s.simple_page()->form() != nullptr; });
    form = s.simple_page()->form();
    REQUIRE(form->editing());
    s.step(QStringLiteral("rename the first program"));
    fields = st::program_fields(form);
    type_into(fields[0], QStringLiteral("Console"));
    s.step(QStringLiteral("click Apply"));
    page = st::create(s);
    st::require_counts(s, {2, 1});
    REQUIRE(page->steps_text().contains(QStringLiteral("Console")));
    s.step(QStringLiteral("change, then go back without applying"));
    click(button(page, QStringLiteral("Change…")));
    wait_for("the form to edit",
             [&] { return s.simple_page()->form() != nullptr; });
    click(named<QPushButton>(s.simple_page()->form(), "jobFormBack"));
    s.settle();
    REQUIRE(st::setup_page(s) != nullptr);
    st::require_counts(s, {2, 1});
    st::remove_shown_setup(s);
    s.finish();
}

TEST_CASE("simple: close one program of a shared device")
{
    Session s("simple: close one program of a shared device");
    st::plug_in_devices(s, {QStringLiteral("COM7")});
    auto* form = st::open_job(s, job::share);
    st::choose_device(form, QStringLiteral("COM7"));
    s.step(QStringLiteral("add a third program so one can be closed"));
    click(button(form, QStringLiteral("Add a program")));
    auto* page = st::create(s);
    st::require_counts(s, {4, 1});
    const vss::link_id link_id = s.fake().links().front().id;
    s.step(QStringLiteral("add a fourth program from the setup page"));
    click(button(page, QStringLiteral("Add a program")));
    s.settle();
    st::require_counts(s, {5, 1});
    REQUIRE_EQ(s.fake().links().front().id, link_id);
    REQUIRE_EQ(s.fake().links().front().targets.size(), qsizetype{4});
    page = st::setup_page(s);
    s.step(QStringLiteral("close the first program port"));
    QPushButton* close = nullptr;
    for (auto* candidate : page->findChildren<QPushButton*>())
    {
        if (candidate->isVisibleTo(page) &&
            candidate->accessibleName().startsWith(QStringLiteral("Close ")))
        {
            close = candidate;
            break;
        }
    }
    REQUIRE(close != nullptr);
    click(close);
    s.settle();
    st::require_counts(s, {4, 1});
    REQUIRE_EQ(s.fake().links().front().id, link_id);
    REQUIRE_EQ(s.fake().links().front().targets.size(), qsizetype{3});
    st::remove_shown_setup(s);
    s.finish();
}

TEST_CASE("simple: see the data on a port")
{
    Session s("simple: see the data on a port");
    s.step(QStringLiteral("nothing is set up: the tile says so"));
    click(s.simple_page()->jobs()->tile(job::port));
    click(named<QPushButton>(s.simple_page()->form(), "jobFormCreate"));
    wait_for("the setup page",
             [&] { return s.simple_page()->setup_page() != nullptr; });
    s.step(QStringLiteral(
        "back to the jobs, then choose 'See the data on a port'"));
    click(button(s.simple_page(), QStringLiteral("Set up something new")));
    auto* watch = accessible(s.simple_page()->jobs(),
                             QStringLiteral("See the data on a port"));
    click(watch);
    s.settle();
    auto* page = st::setup_page(s);
    REQUIRE(page->prefs().show_data);
    REQUIRE(named<QWidget>(page, "setupData")->isVisible());
    s.step(QStringLiteral("type to the port and see it in the data"));
    auto* input = named<QLineEdit>(page, "setupInput");
    type_into(input, QStringLiteral("hello"));
    key(input, Qt::Key_Return);
    s.settle();
    REQUIRE_EQ(s.fake().sent().size(), 1);
    // The in-memory backend raises no events of its own. What the page lists
    // is what arrives at the setup's ports: deliver what the hub would.
    deliver_traffic(s, page->type_target(), QByteArray("hello"),
                    vss::data_direction::rx);
    REQUIRE(page->data_text().contains(QStringLiteral("hello")));
    s.step(QStringLiteral("hide the data"));
    click(button(page, QStringLiteral("Hide")));
    s.settle();
    REQUIRE(!page->prefs().show_data);
    st::remove_shown_setup(s);
    s.finish();
}

TEST_CASE("simple: check this PC is ready")
{
    Session s("simple: check this PC is ready");
    s.pilot().expect(
        "the setup check", [](QWidget* d)
        { return d->windowTitle() == QStringLiteral("Setup check"); },
        [](QWidget* d)
        {
            REQUIRE(has_button(d, QStringLiteral("Re-check")));
            click(button(d, QStringLiteral("Close")));
        });
    s.step(QStringLiteral("choose 'Check this PC is ready'"));
    auto* tile = accessible(s.simple_page()->jobs(),
                            QStringLiteral("Check this PC is ready"));
    click(tile);
    s.settle();
    s.finish();
}

TEST_CASE("simple: start from an example, then remove everything")
{
    Session s("simple: start from an example, then remove everything");
    s.pilot().expect_type<TemplatesDialog>(
        "the examples",
        [](QWidget* d)
        {
            click(accessible(d, QStringLiteral("Test bench, no hardware")));
            click(button(d, QStringLiteral("Use template")));
        });
    s.step(QStringLiteral("click 'Start from an example'"));
    click(button(s.simple_page()->jobs(),
                 QStringLiteral("Start from an example…")));
    s.settle();
    s.pilot().check();
    st::require_counts(s, {2, 1});
    s.step(QStringLiteral("add a pair of programs as well"));
    st::open_job(s, job::pair);
    st::create(s);
    st::require_counts(s, {4, 2});
    s.step(QStringLiteral("click Remove all"));
    s.pilot().expect(
        "confirm Remove all",
        [](QWidget* d) { return has_button(d, QStringLiteral("Remove all")); },
        [](QWidget* d) { click(button(d, QStringLiteral("Remove all"))); });
    click(named<QPushButton>(s.simple_page(), "simpleRemoveAll"));
    s.settle();
    s.pilot().check();
    st::require_counts(s, {0, 0});
    REQUIRE(
        !named<QPushButton>(s.simple_page(), "simpleRemoveAll")->isEnabled());
    s.finish();
}

TEST_CASE("simple: cancelling leaves nothing behind")
{
    Session s("simple: cancelling leaves nothing behind");
    s.step(QStringLiteral("open the pair form, then cancel"));
    st::open_job(s, job::pair);
    click(button(s.simple_page()->form(), QStringLiteral("Cancel")));
    s.settle();
    st::require_counts(s, {0, 0});
    REQUIRE(s.simple_page()->jobs()->isVisible());
    s.step(QStringLiteral("open the examples, then cancel"));
    s.pilot().expect_type<TemplatesDialog>(
        "the examples",
        [](QWidget* d) { click(button(d, QStringLiteral("Cancel"))); });
    click(button(s.simple_page()->jobs(),
                 QStringLiteral("Start from an example…")));
    s.settle();
    s.pilot().check();
    st::require_counts(s, {0, 0});
    s.step(QStringLiteral("open a saved setup, then cancel the file dialog"));
    s.pilot().expect(
        "the file dialog",
        [](QWidget* d) { return has_button(d, QStringLiteral("Open")); },
        [](QWidget* d) { click(button(d, QStringLiteral("Cancel"))); });
    click(
        button(s.simple_page()->jobs(), QStringLiteral("Open a saved setup…")));
    s.settle();
    s.pilot().check();
    st::require_counts(s, {0, 0});
    s.finish();
}
