//
// Copyright (c) 2026 Virtual Serial Studio contributors
//
// SPDX-License-Identifier: GPL-3.0-or-later
//
// Everything the menus offer, chosen the way a person chooses it, and what
// each entry opens looked at in turn; and the editors that sit behind the
// View menu.
//

#include "advanced_steps.hpp"
#include "simple_steps.hpp"

#include "advanced/json_editor.hpp"
#include "qt_wait.hpp"

#include <QApplication>
#include <QClipboard>
#include <QFileDialog>
#include <QMenuBar>
#include <QPlainTextEdit>
#include <QRegularExpression>

#include <vector>

namespace
{

using namespace vss::gui;
using namespace vss::gui::ui;
namespace ad = vss::gui::ui::adv;
namespace st = vss::gui::ui::steps;
using vss::gui::simple::job;

vss::endpoint_id make(Session& s, const char* tile, const char* name)
{
    return ad::add_named(s, QString::fromLatin1(tile),
                         QString::fromLatin1(name));
}

QString plain_text(QString text)
{
    text.remove(QLatin1Char('&'));
    return text;
}

/// One menu entry: the titles to open, from the menu bar down.
using entry_path = std::vector<QString>;

void collect(QMenu* menu, const entry_path& prefix,
             std::vector<entry_path>& out)
{
    for (QAction* entry : menu->actions())
    {
        if (entry->isSeparator() || !entry->isEnabled())
            continue;
        entry_path path = prefix;
        path.push_back(plain_text(entry->text()));
        if (entry->menu() != nullptr)
            collect(entry->menu(), path, out);
        else
            out.push_back(path);
    }
}

std::vector<entry_path> all_entries(Session& s)
{
    std::vector<entry_path> out;
    for (QAction* title : s.window().menuBar()->actions())
    {
        if (title->menu() != nullptr)
            collect(title->menu(), {plain_text(title->text())}, out);
    }
    return out;
}

/// Entries that would end the program, or change the machine.
bool avoided(const entry_path& path)
{
    const QString& last = path.back();
    return last == QLatin1String("Quit") || last == QLatin1String("New") ||
           last.contains(QLatin1String("administrator"), Qt::CaseInsensitive);
}

/// Chooses an entry, opening submenus on the way.
void choose_path(Session& s, const entry_path& path)
{
    auto* bar = s.window().menuBar();
    QAction* title = nullptr;
    for (QAction* candidate : bar->actions())
    {
        if (plain_text(candidate->text()) == path.front())
            title = candidate;
    }
    if (title == nullptr || title->menu() == nullptr)
        return;
    // An earlier entry may have changed what this one can do.
    std::vector<entry_path> now;
    collect(title->menu(), {path.front()}, now);
    bool still_there = false;
    for (const entry_path& candidate : now)
        still_there = still_there || candidate == path;
    if (!still_there)
        return;
    if (path.size() == 2)
    {
        choose_menu(s, path.front(), path.back());
        return;
    }
    Probe::instance().beat();
    QTest::mouseClick(bar, Qt::LeftButton, {},
                      bar->actionGeometry(title).center());
    QApplication::processEvents();
    QMenu* menu = title->menu();
    for (std::size_t level = 1; level + 1 < path.size(); ++level)
        menu = open_submenu(menu, path[level]);
    choose_entry(menu, path.back());
}

/// Walks every entry of every menu once, answering each dialog it opens by
/// trying its controls and closing it. Returns how many entries it chose.
int walk_menus(Session& s)
{
    s.pilot().fallback(
        [](QWidget* dialog)
        {
            // A file dialog is only to be closed: its Save would write.
            if (qobject_cast<QFileDialog*>(dialog) != nullptr)
                return;
            exercise(dialog,
                     {QStringLiteral("Remove"), QStringLiteral("Delete"),
                      QStringLiteral("Install"),
                      QStringLiteral("administrator"),
                      QStringLiteral("Restart"), QStringLiteral("Quit"),
                      QStringLiteral("Revoke"), QStringLiteral("Reset"),
                      QStringLiteral("Fix it"), QStringLiteral("Allow"),
                      QStringLiteral("Open Windows"), QStringLiteral("rule")});
        });
    int chosen = 0;
    for (const entry_path& path : all_entries(s))
    {
        if (avoided(path))
            continue;
        QString name;
        for (const QString& part : path)
            name += part + QStringLiteral(" > ");
        name.chop(3);
        s.step(QStringLiteral("choose ") + name);
        // Restyling every widget at once, as a theme or a density does.
        if (name.contains(QLatin1String("Theme")) ||
            name.contains(QLatin1String("Density")))
            Probe::instance().allow_bulk(10);
        choose_path(s, path);
        s.settle();
        ++chosen;
        // Whatever the entry left behind (a dock, a dialog) is the next
        // entry's starting point; the window is still the active one.
        s.window().activateWindow();
    }
    s.pilot().check();
    return chosen;
}

} // namespace

TEST_CASE("actions: every menu entry in Simple mode")
{
    Session s("actions: every menu entry in Simple mode");
    st::open_job(s, job::pair);
    st::create(s);
    st::require_counts(s, {2, 1});
    const int chosen = walk_menus(s);
    REQUIRE(chosen >= 15);
    s.finish();
}

TEST_CASE("actions: every menu entry in Advanced mode")
{
    Session s("actions: every menu entry in Advanced mode",
              Session::mode::advanced);
    const auto a = make(s, "Port", "a");
    const auto b = make(s, "Fixture", "b");
    ad::wire(s, a, b);
    ad::click_node(s, a);
    const int chosen = walk_menus(s);
    REQUIRE(chosen >= 30);
    REQUIRE(!s.pilot().visited().isEmpty());
    s.finish();
}

TEST_CASE("actions: edit the setup as JSON and apply it")
{
    Session s("actions: edit the setup as JSON and apply it",
              Session::mode::advanced);
    const auto a = make(s, "Port", "a");
    const auto b = make(s, "Port", "b");
    ad::wire(s, a, b);
    s.step(QStringLiteral("View > Panes > Topology JSON"));
    choose_menu_path(s, {QStringLiteral("View"), QStringLiteral("Panes"),
                         QStringLiteral("Topology JSON")});
    s.settle();
    QWidget* dock = s.dock("jsonDock");
    auto* editor = dock->findChild<QPlainTextEdit*>();
    REQUIRE(editor != nullptr);
    // The editor is a tab behind the Monitor; the menu command has to bring it
    // to the front.
    if (editor->visibleRegion().isEmpty())
        throw vss_test::failure(
            "View > Panes > Topology JSON left the editor behind the Monitor tab");
    s.step(QStringLiteral("rename port a in the text and apply"));
    QString text = editor->toPlainText();
    REQUIRE(text.contains(QStringLiteral("\"a\"")));
    text.replace(QStringLiteral("\"a\""), QStringLiteral("\"zed\""));
    click(editor);
    key(editor, Qt::Key_A, Qt::ControlModifier);
    QApplication::clipboard()->setText(text);
    key(editor, Qt::Key_V, Qt::ControlModifier);
    s.settle();
    REQUIRE(editor->toPlainText().contains(QStringLiteral("zed")));
    wait_for(
        "Apply changes to turn on", [dock]
        { return button(dock, QStringLiteral("Apply changes"))->isEnabled(); });
    click(button(dock, QStringLiteral("Apply changes")));
    s.settle();
    s.backend().refresh();
    s.settle();
    bool renamed = false;
    for (const auto& port : s.backend().ports())
        renamed = renamed || port.name == QStringLiteral("zed");
    REQUIRE(renamed);
    ad::require_consistent(s);
    s.step(QStringLiteral(
        "break the text: Apply is refused and Revert goes back"));
    key(editor, Qt::Key_A, Qt::ControlModifier);
    QApplication::clipboard()->setText(QStringLiteral("{ not json"));
    key(editor, Qt::Key_V, Qt::ControlModifier);
    s.settle();
    REQUIRE(!button(dock, QStringLiteral("Apply changes"))->isEnabled());
    wait_for("Revert to turn on", [dock]
             { return button(dock, QStringLiteral("Revert"))->isEnabled(); });
    click(button(dock, QStringLiteral("Revert")));
    s.settle();
    REQUIRE(editor->toPlainText().contains(QStringLiteral("zed")));
    s.finish();
}

TEST_CASE("actions: the JSON editor explains each kind of mistake")
{
    Session s("actions: the JSON editor explains each kind of mistake",
              Session::mode::advanced);
    const auto a = make(s, "Port", "a");
    const auto b = make(s, "Port", "b");
    ad::wire(s, a, b);
    choose_menu_path(s, {QStringLiteral("View"), QStringLiteral("Panes"),
                         QStringLiteral("Topology JSON")});
    s.settle();
    QWidget* dock = s.dock("jsonDock");
    auto* editor = dock->findChild<QPlainTextEdit*>();
    auto* checker = dock->findChild<TopologyJsonEditor*>();
    REQUIRE(editor != nullptr);
    REQUIRE(checker != nullptr);
    const QString valid = editor->toPlainText();
    REQUIRE(valid.contains(QStringLiteral("baud_rate")));
    const auto paste = [&](const QString& text)
    {
        click(editor);
        key(editor, Qt::Key_A, Qt::ControlModifier);
        QApplication::clipboard()->setText(text);
        key(editor, Qt::Key_V, Qt::ControlModifier);
        s.settle();
        vss_test::spin(150);
    };
    QRegularExpression baud(
        QStringLiteral("\"baud_rate\"[ \t\r\n]*:[ \t\r\n]*[0-9]+"));
    struct mistake
    {
        const char* what;
        QString text;
    };
    QString wrong_type = valid;
    wrong_type.replace(baud, QStringLiteral("\"baud_rate\": \"fast\""));
    QString huge = valid;
    huge.replace(baud, QStringLiteral("\"baud_rate\": 99999999999"));
    QString unknown_port = valid;
    unknown_port.replace(QStringLiteral("\"b\""), QStringLiteral("\"nobody\""));
    QString cut = valid;
    cut.chop(3);
    QString no_ports = valid;
    no_ports.replace(QStringLiteral("\"ports\""), QStringLiteral("\"porch\""));
    const mistake mistakes[] = {
        {"empty", QString()},
        {"not JSON", QStringLiteral("{ not json")},
        {"cut off", cut},
        {"a string for a number", wrong_type},
        {"a speed too large", huge},
        {"a link to a port that is not there", unknown_port},
        {"a misspelt section", no_ports},
        {"a list, not an object", QStringLiteral("[1, 2, 3]")},
    };
    int rejected = 0;
    for (const mistake& item : mistakes)
    {
        s.step(QStringLiteral("paste: ") + QString::fromLatin1(item.what));
        paste(item.text);
        if (!checker->can_apply())
        {
            ++rejected;
            REQUIRE(!checker->error_summary().isEmpty() || item.text.isEmpty());
        }
        s.step(QStringLiteral("Format and Revert after: ") +
               QString::fromLatin1(item.what));
        if (button(dock, QStringLiteral("Format"))->isEnabled())
            click(button(dock, QStringLiteral("Format")));
        if (button(dock, QStringLiteral("Revert"))->isEnabled())
            click(button(dock, QStringLiteral("Revert")));
        s.settle();
    }
    REQUIRE(rejected >= 5);
    s.finish();
}
