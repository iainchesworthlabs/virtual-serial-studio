//
// Copyright (c) 2026 Virtual Serial Studio contributors
//
// SPDX-License-Identifier: GPL-3.0-or-later
//
// The Plugins page and its dialogs
// (docs/gui-design/docs/screens/10-plugins.md), against the fake plugin
// backend, which can report any state and records every request. No plugin
// is started anywhere in these tests.
//

#include "qt_input.hpp"
#include "qt_wait.hpp"
#include "test_framework.hpp"

#include "backend/fake_plugin_backend.hpp"
#include "support/decoded_dialog.hpp"
#include "support/licence_text.hpp"
#include "support/plugin_dialogs.hpp"
#include "support/plugin_text.hpp"
#include "support/plugins_dialog.hpp"
#include "theme/theme.hpp"
#include "widgets/controls.hpp"
#include "widgets/dialogs.hpp"
#include "widgets/notes.hpp"

#include <QAccessible>
#include <QApplication>
#include <QCheckBox>
#include <QClipboard>
#include <QComboBox>
#include <QDir>
#include <QFrame>
#include <QGuiApplication>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QLocale>
#include <QMenu>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QScrollArea>
#include <QScrollBar>
#include <QTableWidget>
#include <QTimer>

#include <cstdlib>
#include <functional>

using namespace vss::gui;
using namespace vss::gui::support;

namespace
{

void settle()
{
    for (int round = 0; round < 4; ++round)
        QApplication::processEvents();
}

/// Runs `handler` on the dialog of type `Dialog` that a modal call opens, then
/// lets the call return. The handler must not use REQUIRE: it runs inside the
/// event loop of `exec()`. If no dialog appears the modal widget is closed.
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

QStringList action_texts(PluginsDialog& dialog)
{
    QStringList texts;
    for (QPushButton* button : dialog.detail()->findChildren<QPushButton*>())
    {
        if (button->objectName().startsWith(QStringLiteral("pluginAction.")))
            texts << button->text();
    }
    return texts;
}

NoteBanner* detail_note(PluginsDialog& dialog)
{
    return dialog.detail()->findChild<NoteBanner*>(
        QStringLiteral("pluginDetailNote"));
}

StatusChip* detail_chip(PluginsDialog& dialog)
{
    return static_cast<StatusChip*>(dialog.detail()->findChild<QLabel*>(
        QStringLiteral("pluginDetailChip")));
}

plugin_view sample_of(const char* id)
{
    for (const plugin_view& each : fake_plugin_samples())
    {
        if (each.id == QString::fromLatin1(id))
            return each;
    }
    return {};
}

struct fixture
{
    FakePluginBackend backend;

    explicit fixture(std::vector<plugin_view> plugins = fake_plugin_samples())
    {
        backend.set_plugins(std::move(plugins));
        backend.flush();
    }

    plugin_view sample(const QString& id, const QString& folder = {}) const
    {
        for (const plugin_view& each : backend.snapshot()->plugins)
        {
            if (each.id == id && (folder.isEmpty() || each.folder == folder))
                return each;
        }
        return {};
    }
};

plugin_view hostile_with(plugin_trust trust)
{
    plugin_view view = fake_plugin_samples().front();
    view.id = QStringLiteral("org.evil.lookalike");
    view.name = QStringLiteral("Virtual Serial Studio");
    view.author = QStringLiteral("Virtual Serial Studio");
    view.folder = QStringLiteral("C:/plugins/lookalike");
    view.trust = trust;
    view.state = plugin_state::off;
    view.note = plugin_note::none;
    return view;
}

void key_click(QWidget* widget, Qt::Key key)
{
    QKeyEvent press(QEvent::KeyPress, key, Qt::NoModifier);
    QApplication::sendEvent(widget, &press);
    QKeyEvent release(QEvent::KeyRelease, key, Qt::NoModifier);
    QApplication::sendEvent(widget, &release);
}

QString accessible_name(QWidget* widget)
{
    QAccessibleInterface* interface =
        QAccessible::queryAccessibleInterface(widget);
    return interface != nullptr ? interface->text(QAccessible::Name)
                                : QString();
}

bool all_text_plain(QWidget& root, const QString& bad_characters)
{
    for (QLabel* label : root.findChildren<QLabel*>())
    {
        for (const QChar letter : label->text())
        {
            if (bad_characters.contains(letter))
                return false;
        }
    }
    return true;
}

/// The characters nothing a plugin says may put on the screen.
QString removed_characters()
{
    return QStringLiteral(
        "\u0007\u001b\u202a\u202b\u202c\u202d\u202e\u2066\u2067\u2068\u2069"
        "\u200e\u200f\u061c");
}

} // namespace

// -- states -------------------------------------------------------------------

TEST_CASE("plugins page: every state shows its chip, its sentence and its "
          "action, as the spec words them")
{
    // The fixture has one plugin in each state the page can show. Open the
    // page and let it lay out before reading anything from it.
    fixture env;
    PluginsDialog dialog(env.backend);
    dialog.show();
    settle();

    // The date in the "expired" sentence is written in the local format, so
    // the expected text is built the same way.
    const QString date =
        QLocale().toString(QDate(2026, 6, 30), QLocale::LongFormat);
    // What a row of the table below expects to see in the detail pane: the
    // chip, the note (empty when there is none) and the action buttons.
    struct row
    {
        const char* id;
        const char* folder;
        const char* chip;
        QString note;
        QStringList actions;
    };
    // The sentence for a plugin that needs a licence is the reason from the
    // licence module followed by what happens to the plugin's settings.
    const QString licence_note =
        reason_sentence(vss::license::reason::no_licence,
                        vss::license::no_licence()) +
        QStringLiteral(" Its settings are kept, and it runs again when a "
                       "licence allows it.");
    // The table follows the states in order: running or starting, off,
    // waiting for a decision, blocked by policy or by a lack of isolation,
    // needing a licence, stopped, and the plugins that cannot be used. The
    // words are the ones the spec gives for each. The first row names its
    // folder, as two plugins share this identifier and the folder tells
    // them apart.
    const std::vector<row> rows = {
        {"com.virtualserialstudio.modbus", "Modbus fixture", "On", {}, {}},
        {"org.example.replay",
         nullptr,
         "Starting",
         QStringLiteral("It stopped unexpectedly. Starting again in 4 seconds "
                        "(attempt 3 of 5)."),
         {QStringLiteral("Start now")}},
        {"org.example.noise",
         nullptr,
         "Off",
         QStringLiteral("Not available in this build"),
         {}},
        {"org.hobby.scanner",
         nullptr,
         "Needs your decision",
         QStringLiteral("You have not decided whether to run this plugin."),
         {QStringLiteral("Review and decide")}},
        {"org.example.changed",
         nullptr,
         "Needs your decision",
         QStringLiteral("This plugin changed since you approved it. Review it "
                        "again before it runs."),
         {QStringLiteral("Review and decide")}},
        {"org.example.blocked",
         nullptr,
         "Blocked",
         QStringLiteral("Your administrator allows only official plugins. This "
                        "one is not official."),
         {}},
        {"org.example.named",
         nullptr,
         "Blocked",
         QStringLiteral("Your administrator has blocked this plugin."),
         {}},
        {"org.example.protect",
         nullptr,
         "Blocked",
         QStringLiteral("Your administrator requires more isolation than this "
                        "computer can give this plugin."),
         {QStringLiteral("Copy details")}},
        {"org.example.isolate",
         nullptr,
         "Not isolated",
         QStringLiteral("Studio cannot keep this plugin apart from your files "
                        "and other programs on this computer, so it will not "
                        "run."),
         {QStringLiteral("Run without isolation..."),
          QStringLiteral("Copy details")}},
        {"org.example.isolate2",
         nullptr,
         "Blocked",
         QStringLiteral("Studio cannot keep this plugin apart from your files "
                        "and other programs on this computer, so it will not "
                        "run. Only your administrator can allow it."),
         {QStringLiteral("Copy details")}},
        {"com.virtualserialstudio.fake",
         nullptr,
         "Not used",
         QStringLiteral("Its identifier is one that only official plugins may "
                        "use, and it is not signed by Virtual Serial Studio. "
                        "It will not run."),
         {QStringLiteral("Copy details")}},
        {"com.virtualserialstudio.decode",
         nullptr,
         "Needs a licence",
         licence_note,
         {QStringLiteral("Licence")}},
        {"org.example.crashy",
         nullptr,
         "Stopped",
         QStringLiteral("It stopped 5 times in 10 minutes, so Studio stopped "
                        "starting it."),
         {QStringLiteral("Start again"), QStringLiteral("Show log")}},
        {"org.example.future",
         nullptr,
         "Not compatible",
         QStringLiteral("This plugin needs a newer version of Studio. This is "
                        "version 0.1.0."),
         {}},
        {"org.example.broken",
         nullptr,
         "Can't be used",
         QStringLiteral("Its signature does not match its files. They may have "
                        "been changed. Get the plugin again from its author."),
         {QStringLiteral("Copy details")}},
        {"org.example.expired",
         nullptr,
         "Can't be used",
         QStringLiteral("This plugin expired on %1. Look for a newer version.")
             .arg(date),
         {}},
        {"org.example.withdrawn",
         nullptr,
         "Can't be used",
         QStringLiteral(
             "This plugin was withdrawn by Virtual Serial Studio. It "
             "will not run."),
         {}},
        {"org.example.invalid",
         nullptr,
         "Can't be used",
         QStringLiteral("Its description file is not valid, so Studio cannot "
                        "tell what it is."),
         {QStringLiteral("Copy details")}},
        {"org.example.missing",
         nullptr,
         "Not installed",
         QStringLiteral("A setup uses this plugin, and it is not installed on "
                        "this computer. Its parts of the setup are kept."),
         {}},
    };
    // Select each plugin in turn and compare what the detail pane shows.
    for (const row& each : rows)
    {
        REQUIRE(dialog.select(QString::fromLatin1(each.id)));
        settle();
        // Every state has a chip.
        StatusChip* chip = detail_chip(dialog);
        REQUIRE(chip != nullptr);
        REQUIRE_EQ(chip->text(), QString::fromLatin1(each.chip));
        // A note is shown only when the row has words for it. Without one
        // there must be no banner at all, not an empty one.
        NoteBanner* note = detail_note(dialog);
        if (each.note.isEmpty())
        {
            REQUIRE(note == nullptr);
        }
        else
        {
            REQUIRE(note != nullptr);
            REQUIRE_EQ(note->text(), each.note);
        }
        // The buttons must match in text and in order, with none extra.
        REQUIRE_EQ(action_texts(dialog), each.actions);
    }

    // Both copies of a plugin are rows: the copy that is not used is selected
    // by its own folder.
    REQUIRE(dialog.select(QStringLiteral("com.virtualserialstudio.modbus"),
                          QStringLiteral("C:/Users/you/Studio/plugins/Modbus "
                                         "fixture (your copy)")));
    settle();
    // The copy that is not used has the chip and the sentence that say so,
    // and the note names the kind of each copy.
    REQUIRE_EQ(detail_chip(dialog)->text(), QStringLiteral("Not used"));
    REQUIRE_EQ(detail_note(dialog)->text(),
               QStringLiteral("Another plugin with the same identifier is "
                              "installed, Official. This one, Unsigned, is "
                              "not used. They are not the same plugin."));
    // Its first action leads to the copy that is used.
    REQUIRE_EQ(action_texts(dialog),
               (QStringList{QStringLiteral("Show the copy that is used"),
                            QStringLiteral("Copy details")}));
}

TEST_CASE("plugins page: the list shows what needs attention first, and a "
          "copy that is not used sits under the one that is")
{
    fixture env;
    PluginsDialog dialog(env.backend);
    dialog.show();
    settle();

    const QStringList keys = dialog.row_keys();
    REQUIRE_EQ(keys.size(),
               static_cast<qsizetype>(fake_plugin_samples().size()));
    // The first rows are the ones that need attention.
    const auto state_at = [&](qsizetype index)
    {
        for (const plugin_view& each : env.backend.snapshot()->plugins)
        {
            if (each.id + QLatin1Char('|') + each.folder == keys[index])
                return each.state;
        }
        return plugin_state::off;
    };
    bool seen_calm = false;
    for (qsizetype index = 0; index < keys.size(); ++index)
    {
        const bool attention = needs_attention(state_at(index));
        if (!attention)
            seen_calm = true;
        REQUIRE(!(seen_calm && attention));
    }
    // The copy that is not used is right under the Modbus fixture.
    qsizetype used = -1;
    qsizetype copy = -1;
    for (qsizetype index = 0; index < keys.size(); ++index)
    {
        if (keys[index].endsWith(QStringLiteral("Modbus fixture")))
            used = index;
        if (keys[index].endsWith(QStringLiteral("(your copy)")))
            copy = index;
    }
    REQUIRE(used >= 0);
    REQUIRE_EQ(copy, used + 1);
    // It has no switch.
    REQUIRE(dialog.row_switch(QStringLiteral("com.virtualserialstudio.modbus"),
                              QStringLiteral("C:/Users/you/Studio/plugins/"
                                             "Modbus fixture (your copy)")) ==
            nullptr);
}

TEST_CASE(
    "plugins page: a row has a name, a switch that can be used only where "
    "the spec allows, and the reason when it cannot")
{
    fixture env;
    PluginsDialog dialog(env.backend);
    dialog.show();
    settle();
    QListWidget* list = dialog.list();
    REQUIRE_EQ(accessible_name(list), QStringLiteral("Installed plugins"));

    bool found_row = false;
    for (int index = 0; index < list->count(); ++index)
    {
        const QString name =
            list->item(index)->data(Qt::AccessibleTextRole).toString();
        if (name == QStringLiteral("Modbus fixture, Official, On"))
            found_row = true;
    }
    REQUIRE(found_row);

    const auto row_switch = [&](const char* id)
    {
        Switch* box = dialog.row_switch(QString::fromLatin1(id));
        REQUIRE(box != nullptr);
        return box;
    };
    // On, and usable.
    Switch* modbus = row_switch("com.virtualserialstudio.modbus");
    REQUIRE(modbus->isChecked());
    REQUIRE(modbus->isEnabled());
    REQUIRE_EQ(modbus->accessibleName(), QStringLiteral("Modbus fixture"));
    // Needs a decision: usable, and off.
    REQUIRE(row_switch("org.hobby.scanner")->isEnabled());
    REQUIRE(!row_switch("org.hobby.scanner")->isChecked());
    // Blocked, can't be used, not compatible, not installed: disabled, with the
    // reason as tooltip and accessible description.
    for (const char* id :
         {"org.example.blocked", "org.example.broken", "org.example.future",
          "org.example.missing", "org.example.isolate"})
    {
        Switch* box = row_switch(id);
        REQUIRE(!box->isEnabled());
        REQUIRE(!box->toolTip().isEmpty());
        REQUIRE_EQ(box->accessibleDescription(), box->toolTip());
    }
    REQUIRE_EQ(
        row_switch("org.example.blocked")->toolTip(),
        QStringLiteral("Your administrator allows only official plugins. "
                       "This one is not official."));
    // A build that cannot run plugins says so on a switch that would start one.
    REQUIRE_EQ(row_switch("org.example.noise")->toolTip(),
               QStringLiteral("Not available in this build"));
}

TEST_CASE("plugins page: only the host's own trust level shows the Virtual "
          "Serial Studio badge")
{
    // A plugin whose name and author say it is Virtual Serial Studio, at every
    // trust level.
    for (const plugin_trust trust :
         {plugin_trust::official, plugin_trust::third_party,
          plugin_trust::unknown_key, plugin_trust::unsigned_plugin})
    {
        fixture env({hostile_with(trust)});
        PluginsDialog dialog(env.backend);
        dialog.show();
        settle();
        const bool badge = dialog.detail()->findChild<QWidget*>(QStringLiteral(
                               "pluginOfficialBadge")) != nullptr;
        REQUIRE_EQ(badge, trust == plugin_trust::official);
        // The trust line says what it is, not what it claims.
        const QString name =
            dialog.list()->item(0)->data(Qt::AccessibleTextRole).toString();
        if (trust == plugin_trust::unsigned_plugin)
            REQUIRE(name.contains(QStringLiteral("Unsigned")));
        if (trust == plugin_trust::unknown_key)
        {
            REQUIRE(name.contains(
                QStringLiteral("Signed by an author you have not approved")));
        }
    }
    // The author of an unofficial plugin is shown as text it gave, and an
    // official plugin's is the host's own wording.
    plugin_view lookalike = hostile_with(plugin_trust::third_party);
    lookalike.key_name = QStringLiteral("Example Instruments");
    REQUIRE_EQ(trust_line(lookalike),
               QStringLiteral("Signed by Example Instruments"));
}

TEST_CASE("plugins page: the build of a plugin is shown with its version")
{
    // A build says which commit it was made from after a plus sign, so that a
    // newer build of the same version can be told from an older one.
    plugin_view built = fake_plugin_samples().front();
    built.trust = plugin_trust::official;
    built.version = QStringLiteral("0.1.0+57.g7ffbece");
    REQUIRE_EQ(row_second_line(built),
               QStringLiteral("Official, version 0.1.0+57.g7ffbece"));
    REQUIRE(copy_details_text(built).contains(
        QStringLiteral("Version: 0.1.0+57.g7ffbece")));

    fixture env({built});
    PluginsDialog dialog(env.backend);
    dialog.show();
    settle();
    dialog.select(built.id);
    settle();
    auto* version = dialog.detail()->findChild<QLabel*>(
        QStringLiteral("pluginDetailVersion"));
    REQUIRE(version != nullptr);
    REQUIRE_EQ(version->text(), QStringLiteral("0.1.0+57.g7ffbece"));
}

TEST_CASE("plugins page: hostile text is shown as plain text and breaks "
          "nothing")
{
    plugin_view hostile = fake_hostile_plugin();
    fixture env({hostile});
    PluginsDialog dialog(env.backend);
    dialog.show();
    settle();
    dialog.select(hostile.id);
    settle();

    // Nothing the plugin said is read as markup or moves the text around.
    REQUIRE(all_text_plain(dialog, removed_characters()));
    auto* name =
        dialog.detail()->findChild<QLabel*>(QStringLiteral("pluginDetailName"));
    REQUIRE(name != nullptr);
    REQUIRE_EQ(name->textFormat(), Qt::PlainText);
    REQUIRE(!name->text().contains(QChar(0x202e)));
    // A very long name is cut, never allowed to widen the dialog.
    REQUIRE(name->width() <= dialog.width());
    REQUIRE(dialog.detail()->sizeHint().width() <= dialog.width());
    // Any label that shows a character that markup uses is plain text.
    for (QLabel* label : dialog.findChildren<QLabel*>())
    {
        if (label->isVisibleTo(&dialog) &&
            label->text().contains(QLatin1Char('<')))
            REQUIRE(label->textFormat() == Qt::PlainText);
    }
    // The markup is there as the characters it was written with.
    const QString description =
        dialog.detail()
            ->findChild<QLabel*>(QStringLiteral("pluginDetailDescription"))
            ->text();
    REQUIRE(description.contains(QStringLiteral("<h1>Big</h1>")));
    // A format placeholder in a name never fills in another part of a
    // sentence: the title of the removal dialog has it literally.
    plugin_view odd = hostile;
    odd.name = QStringLiteral("%1 %2 %99");
    PluginRemoveDialog remove(env.backend, odd);
    remove.show();
    settle();
    REQUIRE_EQ(remove.windowTitle(), QStringLiteral("Remove %1 %2 %99?"));
    // Its sentences in a note are the text it was given, not markup.
    REQUIRE(plain_text(QStringLiteral("a\u202eb\u0007c\r\nd")) ==
            QStringLiteral("abcd"));
    // Its log lines: control characters and overrides are gone.
    auto* log = dialog.detail()->findChild<QPlainTextEdit*>(
        QStringLiteral("pluginDetailLogText"));
    REQUIRE(log != nullptr);
    REQUIRE(!log->toPlainText().contains(QChar(0x1b)));
    REQUIRE(!log->toPlainText().contains(QChar(0x202e)));
    REQUIRE(log->toPlainText().contains(QStringLiteral("<script>")));
    // The "Copy details" text is plain too.
    REQUIRE(!dialog.details_text().contains(QChar(0x202e)));
    // And the decision dialog for it.
    PluginDecisionDialog decision(env.backend, hostile,
                                  PluginDecisionDialog::mode::decide);
    decision.show();
    settle();
    REQUIRE(all_text_plain(decision, removed_characters()));
    REQUIRE(decision.width() <= 700);
}

// -- the dialog's own pieces --------------------------------------------------

TEST_CASE("plugins page: the header chip and the note on top follow the rules "
          "of the spec")
{
    fixture env;
    PluginsDialog dialog(env.backend);
    dialog.show();
    settle();
    // Some plugins need attention: the chip says how many.
    REQUIRE(dialog.header_chip()->text().contains(QStringLiteral("need "
                                                                 "attention")));
    REQUIRE(!dialog.top_banner()->isVisibleTo(&dialog));

    // Only one note at a time, in the order of the spec.
    plugin_snapshot snapshot = *env.backend.snapshot();
    snapshot.plugins = {fake_plugin_samples().front()};
    snapshot.banner = plugin_banner::policy_untrusted;
    snapshot.policy_detail = QStringLiteral("not trusted: others can write it");
    env.backend.set_snapshot(snapshot);
    env.backend.flush();
    settle();
    REQUIRE(dialog.top_banner()->isVisibleTo(&dialog));
    REQUIRE_EQ(dialog.top_banner()->text(),
               QStringLiteral("A policy file was found on this computer and "
                              "could not be trusted, so only official plugins "
                              "will run."));
    REQUIRE(dialog.top_banner()->get_kind() == NoteBanner::kind::warn);
    REQUIRE_EQ(dialog.top_banner()->action_button()->text(),
               QStringLiteral("Copy details"));
    QGuiApplication::clipboard()->clear();
    dialog.top_banner()->action_button()->click();
    REQUIRE_EQ(QGuiApplication::clipboard()->text(),
               QStringLiteral("not trusted: others can write it"));

    snapshot.banner = plugin_banner::official_only;
    env.backend.set_snapshot(snapshot);
    env.backend.flush();
    REQUIRE_EQ(dialog.top_banner()->text(),
               QStringLiteral("Your administrator allows only official plugins "
                              "on this computer."));
    snapshot.banner = plugin_banner::user_plugins_off;
    snapshot.user_plugins_allowed = false;
    env.backend.set_snapshot(snapshot);
    env.backend.flush();
    REQUIRE_EQ(dialog.top_banner()->text(),
               QStringLiteral("Your administrator installs the plugins on this "
                              "computer. Plugins in your own plugin folder are "
                              "not used."));
    // "Add a plugin..." and "Open the plugin folder" are off, with the reason.
    // The second is an entry of the "More options" menu, so its reason is its
    // tooltip and its status text, which assistive technology reads.
    const QString administrator_reason = QStringLiteral(
        "Your administrator installs the plugins on this computer.");
    REQUIRE(!dialog.add_button()->isEnabled());
    REQUIRE(!dialog.folder_action()->isEnabled());
    REQUIRE_EQ(dialog.add_button()->toolTip(), administrator_reason);
    REQUIRE_EQ(dialog.folder_action()->toolTip(), administrator_reason);
    REQUIRE_EQ(dialog.folder_action()->statusTip(), administrator_reason);
    // The other two entries still work: they do not touch the user's folder.
    REQUIRE(dialog.import_key_action()->isEnabled());
    REQUIRE(dialog.check_action()->isEnabled());
    snapshot.banner = plugin_banner::no_licence_for_official;
    snapshot.user_plugins_allowed = true;
    env.backend.set_snapshot(snapshot);
    env.backend.flush();
    REQUIRE_EQ(dialog.top_banner()->text(),
               QStringLiteral("Official plugins need a Pro licence."));
    int licence_asked = 0;
    QObject::connect(&dialog, &PluginsDialog::licence_requested, &dialog,
                     [&licence_asked] { ++licence_asked; });
    dialog.top_banner()->action_button()->click();
    REQUIRE_EQ(licence_asked, 1);
    snapshot.banner = plugin_banner::none;
    env.backend.set_snapshot(snapshot);
    env.backend.flush();
    REQUIRE(!dialog.top_banner()->isVisibleTo(&dialog));
    // One plugin is on: the chip says so.
    REQUIRE_EQ(dialog.header_chip()->text(), QStringLiteral("1 on"));
}

TEST_CASE("plugins page: an empty list, a filter and Check again")
{
    fixture env(std::vector<plugin_view>{});
    PluginsDialog dialog(env.backend);
    dialog.show();
    settle();
    auto* title =
        dialog.findChild<QLabel*>(QStringLiteral("pluginsEmptyTitle"));
    auto* text = dialog.findChild<QLabel*>(QStringLiteral("pluginsEmptyText"));
    REQUIRE(title != nullptr);
    REQUIRE(title->isVisibleTo(&dialog));
    REQUIRE_EQ(title->text(), QStringLiteral("No plugins are installed"));
    REQUIRE_EQ(text->text(),
               QStringLiteral("Plugins are folders that you or your "
                              "administrator put in the plugin folder. Studio "
                              "does not download anything."));
    REQUIRE(dialog.findChild<QPushButton*>(QStringLiteral("pluginsEmptyOpen"))
                ->isVisibleTo(&dialog));

    env.backend.set_plugins(fake_plugin_samples());
    env.backend.flush();
    settle();
    REQUIRE(dialog.list()->isVisibleTo(&dialog));
    REQUIRE_EQ(dialog.filter()->placeholderText(),
               QStringLiteral("Filter plugins"));
    REQUIRE_EQ(accessible_name(dialog.filter()),
               QStringLiteral("Filter plugins"));
    dialog.filter()->setText(QStringLiteral("zzz"));
    settle();
    REQUIRE(!dialog.list()->isVisibleTo(&dialog));
    REQUIRE_EQ(text->text(), QStringLiteral("No plugin matches zzz."));
    dialog.filter()->setText(QStringLiteral("loop"));
    settle();
    int shown = 0;
    for (int index = 0; index < dialog.list()->count(); ++index)
        shown += dialog.list()->item(index)->isHidden() ? 0 : 1;
    REQUIRE_EQ(shown, 1);

    // Check again rescans, and says how many it found.
    dialog.filter()->clear();
    env.backend.clear_requests();
    dialog.check_action()->trigger();
    REQUIRE_EQ(env.backend.count_of(plugin_request_kind::rescan), 1);
    env.backend.flush();
    settle();
}

namespace
{

plugin_inspection_view inspection_of(const char* id, const QString& folder)
{
    plugin_inspection_view found;
    found.folder = folder;
    found.plugin = sample_of(id);
    found.failure = plugin_inspection_view::problem::none;
    found.can_install = found.plugin.state != plugin_state::cannot_be_used;
    return found;
}

} // namespace

namespace
{

/// Cancels the install dialog: the folder was looked at and nothing was
/// copied.
void cancel_the_add(fixture& env, PluginsDialog& dialog)
{
    when_shown<PluginInstallDialog>([](PluginInstallDialog& each)
                                    { each.cancel_button()->click(); });
    env.backend.clear_requests();
    dialog.add_button()->click();
    REQUIRE(vss_test::wait_until(
        [&] { return !env.backend.inspected().isEmpty(); }));
    for (int round = 0; round < 50; ++round)
    {
        env.backend.flush();
        settle();
    }
    REQUIRE_EQ(env.backend.inspected().size(), 1);
    REQUIRE(env.backend.requests().empty());
}

/// Confirms the install dialog: the host is asked to add exactly that folder.
void confirm_the_add(fixture& env, PluginsDialog& dialog, const QString& chosen)
{
    when_shown<PluginInstallDialog>([](PluginInstallDialog& each)
                                    { each.add_button()->click(); });
    dialog.add_button()->click();
    REQUIRE(vss_test::wait_until(
        [&]
        {
            return env.backend.count_of(plugin_request_kind::add_plugin) == 1;
        }));
    REQUIRE_EQ(env.backend.requests()[0].path, chosen);
}

} // namespace

TEST_CASE("plugins page: Add a plugin looks first, shows what it found, and "
          "copies only when the person confirms")
{
    fixture env;
    PluginsDialog dialog(env.backend);
    QString chosen = QStringLiteral("C:/somewhere/plugin");
    dialog.set_folder_picker([&chosen] { return chosen; });
    dialog.show();
    settle();
    env.backend.set_inspection(chosen,
                               inspection_of("org.hobby.scanner", chosen));

    cancel_the_add(env, dialog);
    confirm_the_add(env, dialog, chosen);

    // A person who chose no folder looks at nothing.
    chosen.clear();
    env.backend.clear_requests();
    const int looked = static_cast<int>(env.backend.inspected().size());
    dialog.add_button()->click();
    REQUIRE_EQ(static_cast<int>(env.backend.inspected().size()), looked);
}

TEST_CASE("install dialog: it shows the verdict, a plugin that cannot be used "
          "or is installed cannot be confirmed, and the command for everyone "
          "is text to copy")
{
    FakePluginBackend backend;
    const QString folder = QStringLiteral("C:/my plugins/tool");

    // A plugin that can be added.
    plugin_inspection_view good = inspection_of("org.example.noise", folder);
    {
        PluginInstallDialog dialog(backend, good);
        dialog.show();
        settle();
        REQUIRE_EQ(dialog.windowTitle(), QStringLiteral("Add Line noise?"));
        REQUIRE(dialog.cancel_button()->isDefault());
        REQUIRE(dialog.cancel_button()->hasFocus());
        REQUIRE(dialog.add_button()->isEnabled());
        REQUIRE_EQ(
            dialog.everyone_command(),
            QStringLiteral("vss_cli plugin install \"C:/my plugins/tool\" "
                           "--for-everyone"));
        auto* version =
            dialog.findChild<QLabel*>(QStringLiteral("installVersion"));
        REQUIRE(version != nullptr);
        REQUIRE_EQ(version->text(), QStringLiteral("1.2.0"));
        REQUIRE_EQ(accessible_name(dialog.findChild<QWidget*>(
                       QStringLiteral("installCommand"))),
                   QStringLiteral("Install command"));
        key_click(&dialog, Qt::Key_Return);
        settle();
        REQUIRE(dialog.result() == QDialog::Rejected);
        REQUIRE(backend.requests().empty());
    }
    // Studio never elevates: nothing but a request to add is ever sent.
    {
        PluginInstallDialog dialog(backend, good);
        dialog.show();
        dialog.add_button()->click();
        REQUIRE_EQ(backend.requests().size(), std::size_t{1});
        REQUIRE(backend.requests()[0].kind == plugin_request_kind::add_plugin);
    }

    // Refused ones: the button is off and a request is never sent.
    backend.clear_requests();
    plugin_inspection_view broken = inspection_of("org.example.broken", folder);
    plugin_inspection_view twice = inspection_of("org.example.noise", folder);
    twice.failure = plugin_inspection_view::problem::already_installed;
    twice.can_install = false;
    plugin_inspection_view nothing;
    nothing.folder = folder;
    plugin_inspection_view admin = inspection_of("org.example.noise", folder);
    admin.failure = plugin_inspection_view::problem::cannot_write;
    admin.can_install = false;
    plugin_inspection_view everyone =
        inspection_of("org.example.noise", folder);
    everyone.failure = plugin_inspection_view::problem::installed_for_everyone;
    everyone.can_install = false;
    plugin_inspection_view bundled =
        inspection_of("org.example.noise", folder);
    bundled.failure = plugin_inspection_view::problem::came_with_studio;
    bundled.can_install = false;
    const std::pair<plugin_inspection_view, QString> refused[] = {
        {broken, QStringLiteral("Studio cannot add this plugin.\nIts signature "
                                "does not match its files. They may have been "
                                "changed. Get the plugin again from its "
                                "author.")},
        {twice,
         QStringLiteral("Line noise is already installed for you. Its files "
                        "are the same as the ones in this folder.")},
        {nothing, QStringLiteral("That folder is not a plugin. A plugin's "
                                 "folder holds a file named plugin.json.")},
        {admin, QStringLiteral("Your administrator installs the plugins on "
                               "this computer.")},
        {everyone,
         QStringLiteral("Line noise is already installed for everyone on this "
                        "computer, and that copy runs. A copy added for you "
                        "only would not run. Ask your administrator to change "
                        "it.")},
        {bundled, QStringLiteral("Line noise came with Studio, and that copy "
                                 "runs. A copy added for you only would not "
                                 "run.")},
    };
    for (const auto& [found, text] : refused)
    {
        PluginInstallDialog dialog(backend, found);
        dialog.show();
        settle();
        REQUIRE_EQ(dialog.banner()->text(), text);
        REQUIRE(dialog.banner()->get_kind() == NoteBanner::kind::danger);
        REQUIRE(!dialog.add_button()->isEnabled());
        dialog.add();
        dialog.add_button()->click();
        REQUIRE(backend.requests().empty());
    }
}

TEST_CASE("install dialog: text from the manifest is shown as plain text, and "
          "it fits at 200 percent")
{
    FakePluginBackend backend;
    plugin_inspection_view found;
    found.folder = QStringLiteral("C:/x");
    found.plugin = fake_hostile_plugin();
    found.failure = plugin_inspection_view::problem::none;
    found.can_install = true;
    PluginInstallDialog dialog(backend, found);
    dialog.show();
    settle();
    REQUIRE(all_text_plain(dialog, removed_characters()));
    for (QLabel* label : dialog.findChildren<QLabel*>())
    {
        if (label->isVisibleTo(&dialog) &&
            label->text().contains(QLatin1Char('<')))
            REQUIRE(label->textFormat() == Qt::PlainText);
    }
    REQUIRE(dialog.width() <= 700);
    REQUIRE(accessible_name(dialog.add_button()) ==
            QStringLiteral("Add plugin"));
    vss_test::scale_text(&dialog, 2.0);
    settle();
    REQUIRE(vss_test::footer_fully_visible(dialog));
    for (QPushButton* button : dialog.findChildren<QPushButton*>())
    {
        if (button->isVisibleTo(&dialog))
            REQUIRE(button->width() >= button->sizeHint().width() - 1);
    }

    // The longer text of a replacement, with the plugin's own text, fits too.
    plugin_inspection_view older = found;
    older.replaces = plugin_replacement::downgrade;
    older.installed_version = QStringLiteral("9.9.9");
    PluginInstallDialog replacing(backend, older);
    replacing.show();
    settle();
    REQUIRE(all_text_plain(replacing, removed_characters()));
    REQUIRE(replacing.width() <= 700);
    vss_test::scale_text(&replacing, 2.0);
    settle();
    REQUIRE(vss_test::footer_fully_visible(replacing));
    for (QPushButton* button : replacing.findChildren<QPushButton*>())
    {
        if (button->isVisibleTo(&replacing))
            REQUIRE(button->width() >= button->sizeHint().width() - 1);
    }
}

TEST_CASE("install dialog: another version of a plugin the user has replaces "
          "it, says so, and asks the host to replace")
{
    FakePluginBackend backend;
    const QString folder = QStringLiteral("C:/plugins/noise");

    struct expected
    {
        plugin_replacement how;
        QString title;
        QString button;
        QString lead;
    };
    const expected cases[] = {
        {plugin_replacement::upgrade, QStringLiteral("Upgrade Line noise?"),
         QStringLiteral("Upgrade plugin"), QString()},
        {plugin_replacement::replace, QStringLiteral("Replace Line noise?"),
         QStringLiteral("Replace plugin"),
         QStringLiteral("Your copy has the same version, 1.0.0, with other "
                        "files.")},
        {plugin_replacement::downgrade,
         QStringLiteral("Replace Line noise with an older version?"),
         QStringLiteral("Replace plugin"),
         QStringLiteral("Your copy is version 1.0.0. This folder holds an "
                        "older version.")},
    };
    for (const expected& each : cases)
    {
        plugin_inspection_view found = inspection_of("org.example.noise", folder);
        found.replaces = each.how;
        found.installed_version = QStringLiteral("1.0.0");
        PluginInstallDialog dialog(backend, found);
        dialog.show();
        settle();
        REQUIRE_EQ(dialog.windowTitle(), each.title);
        REQUIRE_EQ(dialog.add_button()->text(), each.button);
        REQUIRE_EQ(accessible_name(dialog.add_button()), each.button);
        REQUIRE(dialog.add_button()->isEnabled());
        REQUIRE(dialog.cancel_button()->isDefault());
        REQUIRE(dialog.cancel_button()->hasFocus());
        // The version the person has is listed beside the new one.
        auto* yours =
            dialog.findChild<QLabel*>(QStringLiteral("installYourVersion"));
        REQUIRE(yours != nullptr);
        REQUIRE_EQ(yours->text(), QStringLiteral("1.0.0"));
        // A copy that is not newer says so first, and is a warning at least.
        if (each.lead.isEmpty())
        {
            REQUIRE(!dialog.banner()->text().contains(QStringLiteral("Your copy")));
        }
        else
        {
            REQUIRE(dialog.banner()->text().startsWith(each.lead));
            REQUIRE(dialog.banner()->get_kind() != NoteBanner::kind::info);
        }
        // Enter alone cancels, and nothing is sent until the button is used.
        backend.clear_requests();
        key_click(&dialog, Qt::Key_Return);
        settle();
        REQUIRE(dialog.result() == QDialog::Rejected);
        REQUIRE(backend.requests().empty());
    }
    for (const expected& each : cases)
    {
        plugin_inspection_view found = inspection_of("org.example.noise", folder);
        found.replaces = each.how;
        found.installed_version = QStringLiteral("1.0.0");
        PluginInstallDialog dialog(backend, found);
        dialog.show();
        backend.clear_requests();
        dialog.add_button()->click();
        REQUIRE_EQ(backend.requests().size(), std::size_t{1});
        REQUIRE(backend.requests()[0].kind == plugin_request_kind::add_plugin);
        REQUIRE_EQ(backend.requests()[0].path, folder);
        REQUIRE(backend.requests()[0].flag);
    }

    // A plugin the user does not have is added, not replaced.
    {
        PluginInstallDialog dialog(backend,
                                   inspection_of("org.example.noise", folder));
        dialog.show();
        backend.clear_requests();
        dialog.add_button()->click();
        REQUIRE_EQ(backend.requests().size(), std::size_t{1});
        REQUIRE(!backend.requests()[0].flag);
        REQUIRE(dialog.findChild<QLabel*>(
                    QStringLiteral("installYourVersion")) == nullptr);
    }
}

TEST_CASE("install dialog: the command for a plugin installed for everyone "
          "replaces that copy")
{
    FakePluginBackend backend;
    plugin_inspection_view everyone =
        inspection_of("org.example.noise", QStringLiteral("C:/my plugins/tool"));
    everyone.failure = plugin_inspection_view::problem::installed_for_everyone;
    everyone.can_install = false;
    PluginInstallDialog dialog(backend, everyone);
    dialog.show();
    settle();
    REQUIRE_EQ(dialog.everyone_command(),
               QStringLiteral("vss_cli plugin install \"C:/my plugins/tool\" "
                              "--for-everyone --replace"));
}

TEST_CASE("plugins page: a plugin the user has is upgraded after the person "
          "confirms, and its row is the one shown")
{
    fixture env;
    PluginsDialog dialog(env.backend);
    const plugin_view scanner = sample_of("org.hobby.scanner");
    const plugin_view noise = sample_of("org.example.noise");
    REQUIRE(dialog.select(scanner.id, scanner.folder));

    QString chosen = QStringLiteral("C:/somewhere/newer-noise");
    dialog.set_folder_picker([&chosen] { return chosen; });
    dialog.show();
    settle();
    plugin_inspection_view found = inspection_of("org.example.noise", chosen);
    found.replaces = plugin_replacement::upgrade;
    found.installed_version = noise.version;
    env.backend.set_inspection(chosen, found);

    when_shown<PluginInstallDialog>([](PluginInstallDialog& each)
                                    { each.add_button()->click(); });
    env.backend.clear_requests();
    dialog.add_button()->click();
    REQUIRE(vss_test::wait_until(
        [&]
        {
            return env.backend.count_of(plugin_request_kind::add_plugin) == 1;
        }));
    REQUIRE_EQ(env.backend.requests()[0].path, chosen);
    REQUIRE(env.backend.requests()[0].flag);
    // No row is new, so the row that was upgraded is selected.
    REQUIRE(dialog.selected() != nullptr);
    REQUIRE_EQ(dialog.selected()->id, noise.id);
}

TEST_CASE("plugins page: a plugin that failed to be added says why")
{
    fixture env;
    PluginsDialog dialog(env.backend);
    dialog.show();
    settle();
    env.backend.fail_next_add(plugin_request_error::already_installed,
                              QStringLiteral("Modbus fixture"));
    env.backend.add_plugin(QStringLiteral("C:/x"), false);
    env.backend.flush();
    settle();
    REQUIRE_EQ(dialog.message_banner()->text(),
               QStringLiteral("Modbus fixture is already installed for you."));
}

TEST_CASE("plugins page: adding a plugin that is installed elsewhere names the "
          "plugin and the place")
{
    fixture env;
    PluginsDialog dialog(env.backend);
    dialog.show();
    settle();
    const std::pair<plugin_request_error, QString> cases[] = {
        {plugin_request_error::installed_for_everyone,
         QStringLiteral("Modbus fixture is already installed for everyone on "
                        "this computer, and that copy runs. A copy added for "
                        "you only would not run. Ask your administrator to "
                        "change it.")},
        {plugin_request_error::came_with_studio,
         QStringLiteral("Modbus fixture came with Studio, and that copy runs. "
                        "A copy added for you only would not run.")},
        {plugin_request_error::already_installed,
         QStringLiteral("Modbus fixture is already installed for you.")},
    };
    for (const auto& [error, text] : cases)
    {
        env.backend.fail_next_add(error, QStringLiteral("Modbus fixture"));
        env.backend.add_plugin(QStringLiteral("C:/x"), false);
        env.backend.flush();
        settle();
        REQUIRE_EQ(dialog.message_banner()->text(), text);
    }
}

namespace
{

/// A copy of one plugin installed in `place`. Every copy has the same hash,
/// so that two of them are identical.
plugin_view copy_in(plugin_installed_for place, const QString& folder,
                    plugin_state state)
{
    plugin_view view = sample_of("org.example.noise");
    view.installed_for = place;
    view.folder = folder;
    view.hash = QString(128, QLatin1Char('a'));
    view.fingerprint = view.hash.left(16);
    view.state = state;
    view.note = state == plugin_state::not_used ? plugin_note::not_used_same
                                                 : plugin_note::none;
    return view;
}

} // namespace

TEST_CASE("plugins page: a plugin in two places shows both copies, which one "
          "runs and where, and removing the user's copy leaves the other")
{
    struct row
    {
        plugin_installed_for used;
        plugin_installed_for own;
        QString note;
        bool can_remove;
    };
    const row rows[] = {
        {plugin_installed_for::everyone, plugin_installed_for::you_only,
         QStringLiteral("Another copy of this plugin, installed for everyone "
                        "on this computer, runs instead. This copy, installed "
                        "for you only, is not used. Remove this copy if you "
                        "do not need it."),
         true},
        {plugin_installed_for::with_studio, plugin_installed_for::you_only,
         QStringLiteral("The copy that came with Studio runs instead. This "
                        "copy, installed for you only, is not used. Remove "
                        "this copy if you do not need it."),
         true},
        {plugin_installed_for::with_studio, plugin_installed_for::everyone,
         QStringLiteral("The copy that came with Studio runs instead. This "
                        "copy, installed for everyone on this computer, is "
                        "not used. Ask your administrator to remove it."),
         false},
    };
    for (const theme_mode mode : {theme_mode::light, theme_mode::dark})
    {
        Theme::instance().set_mode(mode);
        for (const row& each : rows)
        {
            const QString used_folder = QStringLiteral("C:/used");
            const QString own_folder = QStringLiteral("C:/own");
            plugin_view used =
                copy_in(each.used, used_folder, plugin_state::available);
            plugin_view own =
                copy_in(each.own, own_folder, plugin_state::not_used);
            own.has_used_copy = true;
            own.used_copy_installed_for = each.used;
            own.used_copy_id = used.id;
            own.used_copy_folder = used_folder;
            fixture env({used, own});
            PluginsDialog dialog(env.backend);
            dialog.show();
            settle();
            // Both copies are rows.
            REQUIRE_EQ(dialog.row_keys().size(), qsizetype{2});
            REQUIRE(dialog.select(used.id, own_folder));
            settle();
            REQUIRE_EQ(detail_note(dialog)->text(), each.note);
            REQUIRE_EQ(detail_chip(dialog)->text(), QStringLiteral("Not used"));
            REQUIRE_EQ(dialog.remove_button()->isEnabled(), each.can_remove);
            REQUIRE(!dialog.grab().isNull());
            // The copy that runs says where it is installed.
            REQUIRE(dialog.select(used.id, used_folder));
            settle();
            const QLabel* where = dialog.detail()->findChild<QLabel*>(
                QStringLiteral("pluginDetailInstalledFor"));
            REQUIRE(where != nullptr);
            REQUIRE_EQ(where->text(), installed_for_text(each.used));
        }
    }
    Theme::instance().set_mode(theme_mode::system);

    // Remove acts on the user's copy even when the other copy is identical,
    // and the other copy stays listed.
    plugin_view used = copy_in(plugin_installed_for::everyone,
                               QStringLiteral("C:/used"),
                               plugin_state::available);
    plugin_view own = copy_in(plugin_installed_for::you_only,
                              QStringLiteral("C:/own"), plugin_state::not_used);
    fixture env({used, own});
    PluginsDialog dialog(env.backend);
    dialog.show();
    settle();
    REQUIRE(dialog.select(used.id, QStringLiteral("C:/own")));
    settle();
    when_shown<PluginRemoveDialog>([](PluginRemoveDialog& each)
                                   { each.remove_button()->click(); });
    dialog.remove_button()->click();
    settle();
    REQUIRE_EQ(env.backend.requests().size(), std::size_t{1});
    env.backend.flush();
    settle();
    REQUIRE_EQ(dialog.row_keys().size(), qsizetype{1});
    REQUIRE(env.backend.snapshot()->plugins[0].installed_for ==
            plugin_installed_for::everyone);
}

// -- the decision dialog ------------------------------------------------------

TEST_CASE("decision dialog: its title, banner and button follow the trust "
          "level, and Cancel is the default")
{
    struct row
    {
        plugin_trust trust;
        QString key_id;
        QDate approved;
        QString title;
        QString banner;
        NoteBanner::kind kind;
        QString run;
    };
    const std::vector<row> rows = {
        {plugin_trust::official,
         QStringLiteral("plugins-2026"),
         {},
         QStringLiteral("Switch on Modbus fixture?"),
         QStringLiteral("Signed by Virtual Serial Studio."),
         NoteBanner::kind::info,
         QStringLiteral("Switch on")},
        {plugin_trust::third_party, QStringLiteral("example-1"),
         QDate(2026, 10, 2),
         QStringLiteral("Run a plugin from Example Instruments?"),
         QStringLiteral("Signed by Example Instruments, whose plugins you have "
                        "approved before. Not verified by Virtual Serial "
                        "Studio."),
         NoteBanner::kind::info, QStringLiteral("Run this plugin")},
        {plugin_trust::third_party,
         QStringLiteral("example-1"),
         {},
         QStringLiteral("Run a plugin from Example Instruments?"),
         QStringLiteral("Signed by Example Instruments. Not verified by "
                        "Virtual Serial Studio. Only run it if you know who "
                        "that is."),
         NoteBanner::kind::warn,
         QStringLiteral("Run this plugin")},
        {plugin_trust::unsigned_plugin,
         {},
         {},
         QStringLiteral("Run an unofficial plugin?"),
         QStringLiteral("Unofficial plugin. It can do anything Studio can.\n"
                        "Studio cannot tell who made it or whether it was "
                        "changed."),
         NoteBanner::kind::danger,
         QStringLiteral("Run anyway")},
        {plugin_trust::unknown_key,
         QStringLiteral("unknown-7"),
         {},
         QStringLiteral("Run an unofficial plugin?"),
         QStringLiteral("Unofficial plugin. It can do anything Studio can.\n"
                        "Studio cannot tell who made it or whether it was "
                        "changed.\nIt is signed with a key that Studio does "
                        "not have (unknown-7), so the signature could not be "
                        "checked."),
         NoteBanner::kind::danger,
         QStringLiteral("Run anyway")},
    };
    for (const row& each : rows)
    {
        FakePluginBackend backend;
        plugin_view plugin = fake_plugin_samples().front();
        plugin.trust = each.trust;
        plugin.key_id = each.key_id;
        plugin.key_name = QStringLiteral("Example Instruments");
        plugin.key_approved_on = each.approved;
        plugin.author = QStringLiteral("Example Instruments");
        plugin.isolation = plugin_isolation::full;
        PluginDecisionDialog dialog(backend, plugin,
                                    PluginDecisionDialog::mode::decide);
        dialog.show();
        settle();
        REQUIRE_EQ(dialog.windowTitle(), each.title);
        REQUIRE_EQ(dialog.trust_banner()->text(), each.banner);
        REQUIRE(dialog.trust_banner()->get_kind() == each.kind);
        REQUIRE_EQ(dialog.run_button()->text(), each.run);
        // Cancel is the default button and has the focus.
        REQUIRE(dialog.cancel_button()->isDefault());
        REQUIRE(!dialog.run_button()->isDefault());
        REQUIRE(dialog.cancel_button()->hasFocus());
        REQUIRE_EQ(dialog.import_key_button() != nullptr,
                   each.trust == plugin_trust::unknown_key);
        // An unofficial plugin's button is the danger one.
        const bool danger = each.trust == plugin_trust::unsigned_plugin ||
                            each.trust == plugin_trust::unknown_key;
        REQUIRE_EQ(dialog.run_button()->property("role").toString(),
                   danger ? QStringLiteral("danger")
                          : QStringLiteral("primary"));
    }
}

TEST_CASE("decision dialog: Enter alone cancels, and a decision is sent with "
          "the plugin's hash and the permissions left on")
{
    FakePluginBackend backend;
    plugin_view plugin = sample_of("org.hobby.scanner");
    REQUIRE_EQ(plugin.id, QStringLiteral("org.hobby.scanner"));
    backend.set_plugins({plugin});
    backend.flush();

    // Enter on the opened dialog answers Cancel, and sends nothing.
    {
        PluginDecisionDialog dialog(backend, plugin,
                                    PluginDecisionDialog::mode::decide);
        dialog.show();
        settle();
        key_click(&dialog, Qt::Key_Return);
        settle();
        REQUIRE(!dialog.isVisible());
        REQUIRE(dialog.result() == QDialog::Rejected);
        REQUIRE(backend.requests().empty());
    }
    // Cancel sends nothing.
    {
        PluginDecisionDialog dialog(backend, plugin,
                                    PluginDecisionDialog::mode::decide);
        dialog.show();
        dialog.cancel_button()->click();
        REQUIRE(backend.requests().empty());
    }
    // Run sends the decision for this hash, with the needed permission and
    // the optional one that is still ticked.
    PluginDecisionDialog dialog(backend, plugin,
                                PluginDecisionDialog::mode::decide);
    dialog.show();
    settle();
    REQUIRE_EQ(dialog.optional_boxes().size(), std::size_t{1});
    REQUIRE(dialog.optional_boxes()[0]->isChecked());
    // The optional permission is a check box, named by its sentence.
    REQUIRE_EQ(dialog.optional_boxes()[0]->accessibleName(),
               QStringLiteral("Write files in a folder you choose."));
    REQUIRE_EQ(dialog.fingerprint_label()->text(), plugin.fingerprint);
    dialog.optional_boxes()[0]->setChecked(false);
    dialog.run_button()->click();
    REQUIRE_EQ(backend.requests().size(), std::size_t{1});
    const fake_plugin_request& sent = backend.requests()[0];
    REQUIRE(sent.kind == plugin_request_kind::decide);
    REQUIRE_EQ(sent.plugin_id, plugin.id);
    REQUIRE_EQ(sent.hash, plugin.hash);
    REQUIRE_EQ(sent.granted, (QStringList{QStringLiteral("ports.read"),
                                          QStringLiteral("process.spawn")}));
    REQUIRE(sent.flag);
    REQUIRE(dialog.result() == QDialog::Accepted);
}

TEST_CASE("decision dialog: the permissions, the isolation line and what a "
          "changed plugin asks for")
{
    FakePluginBackend backend;
    plugin_view changed = sample_of("org.example.changed");
    REQUIRE_EQ(changed.id, QStringLiteral("org.example.changed"));
    PluginDecisionDialog dialog(backend, changed,
                                PluginDecisionDialog::mode::decide);
    dialog.show();
    settle();
    REQUIRE(dialog.changed_banner()->isVisibleTo(&dialog));
    REQUIRE_EQ(dialog.changed_banner()->text(),
               QStringLiteral("You allowed an earlier version of this plugin. "
                              "Look at what changed before you allow this "
                              "one."));
    REQUIRE(dialog.more_banner()->isVisibleTo(&dialog));
    REQUIRE_EQ(dialog.more_banner()->text(),
               QStringLiteral("It asks for more than before:\nConnect to "
                              "updates.example.org."));
    // The plugin's own reason is a quotation, never part of Studio's sentence.
    bool quoted_reason = false;
    for (QLabel* label : dialog.findChildren<QLabel*>())
    {
        if (label->text() ==
            QString::fromUtf8("The plugin says: \xe2\x80\x9cTo check for "
                              "updates.\xe2\x80\x9d"))
            quoted_reason = true;
    }
    REQUIRE(quoted_reason);
    REQUIRE_EQ(dialog.isolation_label()->text(),
               QStringLiteral("Studio runs it apart from your files and other "
                              "programs."));

    // The isolation line, by what this computer can give.
    const auto line = [&](plugin_trust trust, plugin_isolation level)
    {
        plugin_view plugin = fake_plugin_samples().front();
        plugin.trust = trust;
        plugin.isolation = level;
        plugin.isolation_missing = QStringLiteral("No network isolation");
        PluginDecisionDialog each(backend, plugin,
                                  PluginDecisionDialog::mode::decide);
        each.show();
        settle();
        return std::pair{each.isolation_label()->text(),
                         each.run_button()->text()};
    };
    REQUIRE_EQ(line(plugin_trust::third_party, plugin_isolation::partial).first,
               QStringLiteral("Studio can only partly keep it apart on this "
                              "computer: No network isolation."));
    REQUIRE_EQ(
        line(plugin_trust::official, plugin_isolation::none).first,
        QStringLiteral("Studio cannot keep it apart on this computer. It "
                       "runs with your own access to files and the "
                       "network."));
    const auto cannot =
        line(plugin_trust::unsigned_plugin, plugin_isolation::none);
    REQUIRE_EQ(cannot.first,
               QStringLiteral("Studio cannot keep it apart on this computer, "
                              "so it will not run here."));
    REQUIRE_EQ(cannot.second, QStringLiteral("Decide anyway"));
}

TEST_CASE("decision dialog: a needed permission is a row, an amber one is "
          "marked, and every one is read with its state")
{
    FakePluginBackend backend;
    plugin_view plugin = sample_of("org.hobby.scanner");
    PluginDecisionDialog dialog(backend, plugin,
                                PluginDecisionDialog::mode::decide);
    dialog.show();
    settle();
    auto* needed =
        dialog.findChild<QFrame*>(QStringLiteral("permission.process.spawn"));
    REQUIRE(needed != nullptr);
    REQUIRE_EQ(needed->accessibleName(),
               QStringLiteral("Start other programs., Needed"));
    // Amber rows: process.spawn and fs.write are among the six.
    REQUIRE_EQ(needed->property("note").toString(), QStringLiteral("warn"));
    for (const QString& id :
         {QStringLiteral("traffic.tap"), QStringLiteral("modifier.inline"),
          QStringLiteral("net.listen"), QStringLiteral("net.outbound"),
          QStringLiteral("fs.write"), QStringLiteral("process.spawn")})
        REQUIRE(is_amber_permission(id));
    REQUIRE(!is_amber_permission(QStringLiteral("ports.read")));

    // A permission sentence for every permission the spec names.
    const std::pair<const char*, const char*> sentences[] = {
        {"ports.read",
         "See your ports and links, their state and their counters."},
        {"traffic.tap", "Read the data that passes through the ports and links "
                        "you use it on."},
        {"ports.write", "Send data from the ports you use it on."},
        {"topology.modify", "Add, remove and connect ports and links."},
        {"modifier.inline",
         "Hold, change or drop data on the links you use it on."},
        {"faults", "Cause line errors and disconnections, and set timed "
                   "replies."},
        {"serial.devices", "See the list of serial devices on this computer."},
        {"fs.read", "Read files in a folder you choose."},
        {"fs.write", "Write files in a folder you choose."},
        {"process.spawn", "Start other programs."},
        {"bundle.section", "Add a section to a support bundle. You see it and "
                           "choose whether to include it."},
    };
    for (const auto& [id, sentence] : sentences)
    {
        plugin_permission_view permission;
        permission.id = QString::fromLatin1(id);
        REQUIRE_EQ(permission_sentence(permission),
                   QString::fromLatin1(sentence));
    }
    plugin_permission_view listen;
    listen.id = QStringLiteral("net.listen");
    listen.scope = {QStringLiteral("127.0.0.1:5020")};
    REQUIRE_EQ(permission_sentence(listen),
               QStringLiteral("Accept connections from programs on this "
                              "computer, on 127.0.0.1:5020."));
    listen.scope = {QStringLiteral("0.0.0.0:5020")};
    REQUIRE_EQ(permission_sentence(listen),
               QStringLiteral("Accept connections on 0.0.0.0:5020."));
    plugin_permission_view outbound;
    outbound.id = QStringLiteral("net.outbound");
    outbound.scope = {QStringLiteral("example.org"), QStringLiteral("a.test")};
    REQUIRE_EQ(permission_sentence(outbound),
               QStringLiteral("Connect to example.org, a.test."));
}

TEST_CASE("decision dialog: a plugin whose version changed while it was open "
          "closes it and sends nothing; other changes leave it open")
{
    FakePluginBackend backend;
    plugin_view plugin = sample_of("org.hobby.scanner");
    backend.set_plugins({plugin});
    backend.flush();
    PluginDecisionDialog dialog(backend, plugin,
                                PluginDecisionDialog::mode::decide);
    dialog.show();
    settle();

    // The host reports another state for the same files: nothing to ask again.
    REQUIRE(backend.change(plugin.id, [](plugin_view& each)
                           { each.back_off_seconds = 3; }));
    backend.flush();
    settle();
    REQUIRE(dialog.isVisible());

    // The files changed: the version this dialog is about is gone.
    REQUIRE(backend.change(plugin.id, [](plugin_view& each)
                           { each.hash = QString(128, QLatin1Char('e')); }));
    backend.flush();
    settle();
    REQUIRE(!dialog.isVisible());
    REQUIRE(dialog.result() == QDialog::Rejected);
    REQUIRE(backend.requests().empty());

    // The same for a plugin that is removed.
    PluginDecisionDialog second(backend, backend.snapshot()->plugins.front(),
                                PluginDecisionDialog::mode::decide);
    second.show();
    settle();
    backend.set_plugins({});
    backend.flush();
    settle();
    REQUIRE(!second.isVisible());
    REQUIRE(backend.requests().empty());
}

TEST_CASE("decision dialog: the switch on the page asks for the decision "
          "first, and Import the author's key... closes it and opens the key "
          "flow")
{
    fixture env;
    PluginsDialog dialog(env.backend);
    dialog.show();
    settle();
    const plugin_view needs = env.sample(QStringLiteral("org.hobby.scanner"));
    Switch* box = dialog.row_switch(needs.id);
    REQUIRE(box != nullptr);

    // Cancel: nothing was sent and the switch is off.
    when_shown<PluginDecisionDialog>([](PluginDecisionDialog& each)
                                     { each.cancel_button()->click(); });
    box->click();
    settle();
    REQUIRE(env.backend.requests().empty());
    REQUIRE(!dialog.row_switch(needs.id)->isChecked());

    // Run: the decision goes with the plugin's hash.
    when_shown<PluginDecisionDialog>([](PluginDecisionDialog& each)
                                     { each.run_button()->click(); });
    box->click();
    settle();
    REQUIRE_EQ(env.backend.requests().size(), std::size_t{1});
    REQUIRE(env.backend.requests()[0].kind == plugin_request_kind::decide);
    REQUIRE_EQ(env.backend.requests()[0].hash, needs.hash);

    // A plugin that needs no decision is switched on with the hash.
    env.backend.clear_requests();
    const plugin_view off = env.sample(QStringLiteral("org.example.noise"));
    dialog.row_switch(off.id)->click();
    REQUIRE_EQ(env.backend.requests().size(), std::size_t{1});
    REQUIRE(env.backend.requests()[0].kind == plugin_request_kind::set_enabled);
    REQUIRE_EQ(env.backend.requests()[0].hash, off.hash);
    REQUIRE(env.backend.requests()[0].flag);

    // "Import the author's key..." in the dialog of a plugin signed with a key
    // that is not on this computer opens the key flow.
    int key_asked = 0;
    dialog.set_key_picker(
        [&key_asked]
        {
            ++key_asked;
            return QString();
        });
    dialog.select(QStringLiteral("org.unknown.thing"));
    when_shown<PluginDecisionDialog>([](PluginDecisionDialog& each)
                                     { each.import_key_button()->click(); });
    dialog.detail()
        ->findChild<QPushButton*>(QStringLiteral("pluginAction.1"))
        ->click();
    REQUIRE(vss_test::wait_until([&] { return key_asked == 1; }));
}

// -- the key dialog -----------------------------------------------------------

TEST_CASE("key dialog: nothing is approved until the fingerprint is "
          "compared, and Enter alone cancels")
{
    FakePluginBackend backend;
    plugin_key_file_view key;
    key.path = QStringLiteral("C:/keys/author.vsskey.json");
    key.author = QStringLiteral("Example Instruments");
    key.key_id = QStringLiteral("example-1");
    key.fingerprint = QStringLiteral("0123456789abcdef");
    {
        PluginKeyImportDialog dialog(backend, key);
        dialog.show();
        settle();
        REQUIRE_EQ(dialog.windowTitle(),
                   QStringLiteral("Approve the key of Example Instruments?"));
        REQUIRE(dialog.cancel_button()->isDefault());
        REQUIRE(dialog.cancel_button()->hasFocus());
        REQUIRE(!dialog.approve_button()->isDefault());
        REQUIRE(!dialog.approve_button()->isEnabled());
        REQUIRE(!dialog.confirm_box()->isChecked());
        // The fingerprint is in groups of four, and is read one character at
        // a time.
        REQUIRE_EQ(dialog.fingerprint_label()->text(),
                   QStringLiteral("0123 4567 89ab cdef"));
        REQUIRE_EQ(dialog.fingerprint_label()->accessibleName(),
                   QStringLiteral("0 1 2 3, 4 5 6 7, 8 9 a b, c d e f"));
        REQUIRE_EQ(dialog.compare_banner()->text(),
                   QStringLiteral("Compare the fingerprint with the one the "
                                  "author published, on their website or in a "
                                  "message from them. If they differ, do not "
                                  "approve the key."));
        REQUIRE(!dialog.inside_banner()->isVisibleTo(&dialog));
        // Approve does nothing while the box is not ticked, and Enter cancels.
        dialog.approve();
        REQUIRE(backend.requests().empty());
        key_click(&dialog, Qt::Key_Return);
        settle();
        REQUIRE(dialog.result() == QDialog::Rejected);
        REQUIRE(backend.requests().empty());
    }
    // Ticked: it can be approved, and the key's file and fingerprint go with
    // the request. No plugin is switched on.
    key.inside_plugin_folder = true;
    PluginKeyImportDialog dialog(backend, key);
    dialog.show();
    settle();
    REQUIRE(dialog.inside_banner()->isVisibleTo(&dialog));
    REQUIRE_EQ(dialog.inside_banner()->text(),
               QStringLiteral("This key file came with the plugin. Anyone who "
                              "changed the plugin could have changed this file "
                              "too, so compare the fingerprint with one from "
                              "somewhere else."));
    dialog.confirm_box()->setChecked(true);
    REQUIRE(dialog.approve_button()->isEnabled());
    dialog.approve_button()->click();
    REQUIRE_EQ(backend.requests().size(), std::size_t{1});
    REQUIRE(backend.requests()[0].kind == plugin_request_kind::approve_key);
    REQUIRE_EQ(backend.requests()[0].path, key.path);
    REQUIRE_EQ(backend.requests()[0].fingerprint, key.fingerprint);
    REQUIRE_EQ(backend.count_of(plugin_request_kind::decide), 0);
    REQUIRE_EQ(backend.count_of(plugin_request_kind::set_enabled), 0);
}

TEST_CASE("key dialog: the page opens it for a key file the host accepts, and "
          "says why for one it does not")
{
    fixture env;
    PluginsDialog dialog(env.backend);
    QString picked;
    dialog.set_key_picker([&picked] { return picked; });
    dialog.show();
    settle();

    using problem = plugin_key_file_view::problem;
    const auto try_file = [&](const QString& path, problem failure)
    {
        plugin_key_file_view key;
        key.failure = failure;
        key.author = QStringLiteral("Example Instruments");
        key.key_id = QStringLiteral("example-1");
        key.fingerprint = QStringLiteral("0123456789abcdef");
        env.backend.set_key_file(path, key);
        picked = path;
        dialog.import_key_action()->trigger();
        env.backend.flush();
        settle();
    };
    try_file(QStringLiteral("C:/keys/a.vsskey.json"), problem::not_a_key_file);
    REQUIRE_EQ(dialog.message_banner()->text(),
               QStringLiteral("That file is not a plugin key file."));
    try_file(QStringLiteral("C:/keys/b.vsskey.json"), problem::id_in_use);
    REQUIRE_EQ(
        dialog.message_banner()->text(),
        QStringLiteral("A key with the id example-1 is already approved. "
                       "An id cannot change its key."));
    try_file(QStringLiteral("C:/keys/c.vsskey.json"),
             problem::id_is_official_or_administrators);
    REQUIRE_EQ(dialog.message_banner()->text(),
               QStringLiteral("The id example-1 belongs to a key that is "
                              "already on this computer."));

    // A name with a control character or a direction mark never gets to the
    // backend.
    env.backend.clear_requests();
    picked = QStringLiteral("C:/keys/ev") + QChar(0x202e) +
             QStringLiteral("il.vsskey.json");
    dialog.import_key_action()->trigger();
    settle();
    REQUIRE_EQ(dialog.message_banner()->text(),
               QStringLiteral("That file is not a plugin key file."));

    // A good file opens the dialog; cancelling it approves nothing.
    when_shown<PluginKeyImportDialog>([](PluginKeyImportDialog& each)
                                      { each.cancel_button()->click(); });
    try_file(QStringLiteral("C:/keys/good.vsskey.json"), problem::none);
    REQUIRE_EQ(env.backend.count_of(plugin_request_kind::approve_key), 0);

    // Approving it tells the page, and a toast names the author.
    when_shown<PluginKeyImportDialog>(
        [](PluginKeyImportDialog& each)
        {
            each.confirm_box()->setChecked(true);
            each.approve_button()->click();
        });
    try_file(QStringLiteral("C:/keys/good.vsskey.json"), problem::none);
    REQUIRE_EQ(env.backend.count_of(plugin_request_kind::approve_key), 1);
}

// -- isolation ----------------------------------------------------------------

TEST_CASE("no-isolation dialog: the override is for this plugin's hash, needs "
          "the box ticked, and Undo takes it back")
{
    FakePluginBackend backend;
    plugin_view plugin = sample_of("org.example.isolate");
    REQUIRE_EQ(plugin.id, QStringLiteral("org.example.isolate"));
    backend.set_plugins({plugin});
    backend.flush();
    PluginNoIsolationDialog dialog(backend, plugin);
    dialog.show();
    settle();
    REQUIRE_EQ(dialog.windowTitle(),
               QStringLiteral("Run Loop tester without isolation?"));
    REQUIRE_EQ(dialog.banner()->text(),
               QStringLiteral("It will run with your own access to your files, "
                              "the network and other programs."));
    REQUIRE(dialog.banner()->get_kind() == NoteBanner::kind::danger);
    REQUIRE(dialog.cancel_button()->isDefault());
    REQUIRE(dialog.cancel_button()->hasFocus());
    REQUIRE(!dialog.run_button()->isEnabled());
    REQUIRE_EQ(dialog.run_button()->text(),
               QStringLiteral("Run without isolation"));
    REQUIRE_EQ(dialog.confirm_box()->text(),
               QStringLiteral("I understand that Studio is not keeping this "
                              "plugin apart"));
    key_click(&dialog, Qt::Key_Return);
    settle();
    REQUIRE(dialog.result() == QDialog::Rejected);
    REQUIRE(backend.requests().empty());

    PluginNoIsolationDialog again(backend, plugin);
    again.show();
    again.run();
    REQUIRE(backend.requests().empty());
    again.confirm_box()->setChecked(true);
    again.run_button()->click();
    REQUIRE_EQ(backend.requests().size(), std::size_t{1});
    REQUIRE(backend.requests()[0].kind ==
            plugin_request_kind::set_no_isolation);
    REQUIRE_EQ(backend.requests()[0].hash, plugin.hash);
    REQUIRE(backend.requests()[0].flag);

    // An unsigned plugin has a second line.
    plugin_view unsigned_plugin = plugin;
    unsigned_plugin.trust = plugin_trust::unsigned_plugin;
    PluginNoIsolationDialog third(backend, unsigned_plugin);
    third.show();
    REQUIRE_EQ(third.banner()->text(),
               QStringLiteral("It will run with your own access to your files, "
                              "the network and other programs.\nStudio cannot "
                              "tell who made it or whether it was changed."));

    // On the page: the isolation row says so and offers Undo.
    backend.change(plugin.id,
                   [](plugin_view& each)
                   {
                       each.override_active = true;
                       each.override_on = QDate(2026, 11, 15);
                       each.state = plugin_state::off;
                       each.note = plugin_note::none;
                   });
    backend.flush();
    PluginsDialog page(backend);
    page.show();
    settle();
    auto* isolation = page.detail()->findChild<QLabel*>(
        QStringLiteral("pluginDetailIsolationText"));
    REQUIRE(isolation != nullptr);
    REQUIRE(
        isolation->text().startsWith(QStringLiteral("None. You allowed this "
                                                    "on ")));
    backend.clear_requests();
    page.detail()
        ->findChild<QPushButton*>(QStringLiteral("pluginDetailUndoIsolation"))
        ->click();
    REQUIRE_EQ(backend.requests().size(), std::size_t{1});
    REQUIRE(!backend.requests()[0].flag);
}

// -- other actions on the page ------------------------------------------------

TEST_CASE("plugins page: Remove plugin asks first, Cancel is the default, and "
          "only a plugin of the user's own can be removed")
{
    fixture env;
    PluginsDialog dialog(env.backend);
    dialog.show();
    settle();
    // Installed for everyone: disabled, with the reason.
    dialog.select(QStringLiteral("org.example.blocked"));
    settle();
    REQUIRE(!dialog.remove_button()->isEnabled());
    REQUIRE_EQ(dialog.remove_button()->toolTip(),
               QStringLiteral("Installed by your administrator"));
    // Came with Studio.
    dialog.select(QStringLiteral("com.virtualserialstudio.modbus"),
                  QStringLiteral("C:/Users/you/Studio/plugins/Modbus fixture"));
    settle();
    REQUIRE(!dialog.remove_button()->isEnabled());
    REQUIRE_EQ(dialog.remove_button()->toolTip(),
               QStringLiteral("Came with Studio"));
    // Installed for the user: asks first.
    dialog.select(QStringLiteral("org.example.noise"));
    settle();
    REQUIRE(dialog.remove_button()->isEnabled());

    PluginRemoveDialog direct(env.backend, *dialog.selected());
    direct.show();
    settle();
    REQUIRE_EQ(direct.windowTitle(), QStringLiteral("Remove Line noise?"));
    REQUIRE(direct.cancel_button()->isDefault());
    REQUIRE(direct.cancel_button()->hasFocus());
    REQUIRE(!direct.delete_data_box()->isChecked());
    REQUIRE_EQ(direct.delete_data_box()->text(),
               QStringLiteral("Also delete its saved data"));
    direct.cancel_button()->click();
    REQUIRE(env.backend.requests().empty());

    when_shown<PluginRemoveDialog>(
        [](PluginRemoveDialog& each)
        {
            each.delete_data_box()->setChecked(true);
            each.remove_button()->click();
        });
    dialog.remove_button()->click();
    settle();
    REQUIRE_EQ(env.backend.requests().size(), std::size_t{1});
    REQUIRE(env.backend.requests()[0].kind ==
            plugin_request_kind::remove_plugin);
    REQUIRE(env.backend.requests()[0].flag);
}

TEST_CASE("plugins page: a state change updates the open page, is announced "
          "once, and keeps the selection")
{
    fixture env;
    PluginsDialog dialog(env.backend);
    dialog.show();
    settle();
    dialog.select(QStringLiteral("org.example.replay"));
    settle();
    REQUIRE_EQ(detail_chip(dialog)->text(), QStringLiteral("Starting"));

    QStringList announced;
    static QStringList* sink = nullptr;
    sink = &announced;
    QAccessible::installUpdateHandler(
        [](QAccessibleEvent* event)
        {
            if (event->type() == QAccessible::Announcement && sink != nullptr)
            {
                *sink << static_cast<QAccessibleAnnouncementEvent*>(event)
                             ->message();
            }
        });
    QAccessible::setActive(true);

    env.backend.change(QStringLiteral("org.example.replay"),
                       [](plugin_view& each)
                       {
                           each.state = plugin_state::available;
                           each.note = plugin_note::none;
                       });
    env.backend.flush();
    settle();
    REQUIRE(dialog.isVisible());
    REQUIRE_EQ(dialog.selected()->id, QStringLiteral("org.example.replay"));
    REQUIRE_EQ(detail_chip(dialog)->text(), QStringLiteral("On"));
    REQUIRE(detail_note(dialog) == nullptr);
    REQUIRE_EQ(announced, QStringList{QStringLiteral("Replay, On")});

    // The same state again says nothing more.
    env.backend.change(QStringLiteral("org.example.replay"),
                       [](plugin_view& each) { each.version = "1.2.1"; });
    env.backend.flush();
    settle();
    REQUIRE_EQ(announced.size(), 1);
    QAccessible::installUpdateHandler(nullptr);
    sink = nullptr;
}

TEST_CASE("plugins page: Copy details holds the plugin's facts and nothing "
          "else")
{
    fixture env;
    PluginsDialog dialog(env.backend);
    dialog.show();
    settle();
    dialog.select(QStringLiteral("org.example.crashy"));
    settle();
    const QString text = dialog.details_text();
    for (const QString& part :
         {QStringLiteral("Plugin: org.example.crashy"),
          QStringLiteral("Version: 1.2.0"), QStringLiteral("State: Stopped"),
          QStringLiteral("Reason code: quarantined"),
          QStringLiteral("Fingerprint: "),
          QStringLiteral("Signer key id: "
                         "example-1"),
          QStringLiteral("Installed for: You only"),
          QStringLiteral("Isolation: Full"),
          QStringLiteral("Message from the host: ")})
        REQUIRE(text.contains(part));
    // Not a log line, not a path.
    REQUIRE(!text.contains(QStringLiteral("Segmentation")));
    REQUIRE(!text.contains(QStringLiteral("C:/")));
    QGuiApplication::clipboard()->clear();
    dialog.copy_details_button()->click();
    REQUIRE_EQ(QGuiApplication::clipboard()->text(), text);
}

// -- keyboard, names, themes, size --------------------------------------------

TEST_CASE("plugins page: the list, the detail and the footer are reached by "
          "keyboard, in that order")
{
    fixture env;
    PluginsDialog dialog(env.backend);
    dialog.show();
    settle();
    // The list has the focus when it opens.
    REQUIRE(dialog.list()->hasFocus());
    // The arrow keys move through the rows, and the detail follows.
    const QString first = dialog.selected()->id + dialog.selected()->folder;
    key_click(dialog.list(), Qt::Key_Down);
    settle();
    REQUIRE(dialog.selected()->id + dialog.selected()->folder != first);

    // F6 goes to the detail and back.
    key_click(&dialog, Qt::Key_F6);
    settle();
    REQUIRE(!dialog.list()->hasFocus());
    REQUIRE(
        dialog.detail()->isAncestorOf(QApplication::focusWidget()) ||
        QApplication::focusWidget() ==
            dialog.findChild<QWidget*>(QStringLiteral("pluginsDetailScroll")));
    key_click(&dialog, Qt::Key_F6);
    settle();
    REQUIRE(dialog.list()->hasFocus());

    // Tab goes from the list to the detail before it reaches the footer.
    QWidget* scroll =
        dialog.findChild<QWidget*>(QStringLiteral("pluginsDetailScroll"));
    REQUIRE(scroll != nullptr);
    bool reached_detail = false;
    bool reached_footer = false;
    for (int step = 0; step < 80 && !reached_footer; ++step)
    {
        key_click(QApplication::focusWidget(), Qt::Key_Tab);
        QWidget* walker = QApplication::focusWidget();
        if (walker == nullptr)
            break;
        if (walker == scroll || dialog.detail()->isAncestorOf(walker))
            reached_detail = true;
        if (walker == dialog.remove_button() ||
            walker == dialog.copy_details_button() ||
            walker == dialog.close_button())
            reached_footer = true;
    }
    REQUIRE(reached_detail);
    REQUIRE(reached_footer);

    // Space on a switch toggles it.
    env.backend.clear_requests();
    Switch* box = dialog.row_switch(QStringLiteral("org.example.noise"));
    REQUIRE(box != nullptr);
    box->setFocus();
    key_click(box, Qt::Key_Space);
    REQUIRE_EQ(env.backend.count_of(plugin_request_kind::set_enabled), 1);
}

TEST_CASE("plugins page: every control has an accessible name and the right "
          "role")
{
    fixture env;
    PluginsDialog dialog(env.backend);
    dialog.show();
    settle();
    const auto check = [](QWidget& root)
    {
        for (QPushButton* button : root.findChildren<QPushButton*>())
        {
            if (!button->isVisibleTo(&root))
                continue;
            REQUIRE(!accessible_name(button).isEmpty());
            QAccessibleInterface* interface =
                QAccessible::queryAccessibleInterface(button);
            REQUIRE(interface != nullptr);
            // A button that opens a menu is a menu button.
            REQUIRE(interface->role() == (button->menu() != nullptr
                                              ? QAccessible::ButtonMenu
                                              : QAccessible::Button));
        }
        for (QCheckBox* box : root.findChildren<QCheckBox*>())
        {
            if (box->isVisibleTo(&root))
                REQUIRE(!accessible_name(box).isEmpty());
        }
        for (QLineEdit* edit : root.findChildren<QLineEdit*>())
        {
            if (edit->isVisibleTo(&root))
                REQUIRE(!accessible_name(edit).isEmpty());
        }
        for (QPlainTextEdit* edit : root.findChildren<QPlainTextEdit*>())
        {
            if (edit->isVisibleTo(&root))
                REQUIRE(!accessible_name(edit).isEmpty());
        }
    };
    for (const plugin_view& each : env.backend.snapshot()->plugins)
    {
        dialog.select(each.id, each.folder);
        settle();
        check(dialog);
    }
    QAccessibleInterface* list =
        QAccessible::queryAccessibleInterface(dialog.list());
    REQUIRE(list != nullptr);
    REQUIRE(list->role() == QAccessible::List);
    dialog.select(QStringLiteral("org.example.crashy"));
    settle();
    auto* log_text = dialog.detail()->findChild<QPlainTextEdit*>(
        QStringLiteral("pluginDetailLogText"));
    REQUIRE(log_text != nullptr);
    REQUIRE_EQ(accessible_name(log_text),
               QStringLiteral("Messages from Crashy"));

    plugin_view plugin = sample_of("org.hobby.scanner");
    PluginDecisionDialog decision(env.backend, plugin,
                                  PluginDecisionDialog::mode::decide);
    decision.show();
    settle();
    check(decision);
    plugin_key_file_view key;
    key.author = QStringLiteral("A");
    key.key_id = QStringLiteral("a-1");
    key.fingerprint = QStringLiteral("0123456789abcdef");
    PluginKeyImportDialog key_dialog(env.backend, key);
    key_dialog.show();
    settle();
    check(key_dialog);
    PluginNoIsolationDialog isolation(env.backend, plugin);
    isolation.show();
    settle();
    check(isolation);
    PluginRemoveDialog remove(env.backend, plugin);
    remove.show();
    settle();
    check(remove);
}

TEST_CASE("plugins page: every button is at least 24 px, and the pages work in "
          "both themes")
{
    for (const theme_mode mode : {theme_mode::light, theme_mode::dark})
    {
        Theme::instance().set_mode(mode);
        fixture env;
        PluginsDialog dialog(env.backend);
        dialog.show();
        settle();
        for (const plugin_view& each : env.backend.snapshot()->plugins)
        {
            dialog.select(each.id, each.folder);
            settle();
            for (QPushButton* button : dialog.findChildren<QPushButton*>())
            {
                if (button->isVisibleTo(&dialog))
                    REQUIRE(button->height() >= 24);
            }
            for (QCheckBox* box : dialog.findChildren<QCheckBox*>())
            {
                if (dynamic_cast<Switch*>(box) != nullptr &&
                    box->isVisibleTo(&dialog))
                    REQUIRE(box->height() >= 24);
            }
        }
        const QPixmap picture = dialog.grab();
        REQUIRE(!picture.isNull());
        // The page is drawn from tokens: the ground colour follows the theme.
        const QColor ground = Theme::instance().colour(colour_role::panel);
        REQUIRE(picture.toImage().pixelColor(2, 2) != QColor());
        static_cast<void>(ground);
    }
    Theme::instance().set_mode(theme_mode::system);
}

TEST_CASE("plugins page: at 200 percent text nothing is clipped and the footer "
          "stays in view")
{
    fixture env;
    PluginsDialog dialog(env.backend);
    dialog.show();
    settle();
    dialog.select(QStringLiteral("org.example.isolate"));
    settle();
    vss_test::scale_text(&dialog, 2.0);
    settle();
    REQUIRE(vss_test::footer_fully_visible(dialog));
    for (QPushButton* button : dialog.findChildren<QPushButton*>())
    {
        if (!button->isVisibleTo(&dialog))
            continue;
        // A button is as wide as its text needs.
        REQUIRE(button->width() >= button->sizeHint().width() - 1);
    }
    // The detail scrolls inside the dialog.
    auto* scroll =
        dialog.findChild<QScrollArea*>(QStringLiteral("pluginsDetailScroll"));
    REQUIRE(scroll != nullptr);
    REQUIRE(scroll->verticalScrollBarPolicy() != Qt::ScrollBarAlwaysOff);
    vss_test::shrink_to_minimum(dialog);
    REQUIRE(vss_test::footer_fully_visible(dialog));

    // The small dialogs too.
    plugin_view plugin = sample_of("org.hobby.scanner");
    plugin_key_file_view key;
    key.author = QStringLiteral("Example Instruments");
    key.key_id = QStringLiteral("example-1");
    key.fingerprint = QStringLiteral("0123456789abcdef");
    key.inside_plugin_folder = true;
    std::vector<QDialog*> dialogs;
    dialogs.push_back(new PluginDecisionDialog(
        env.backend, plugin, PluginDecisionDialog::mode::decide));
    dialogs.push_back(new PluginKeyImportDialog(env.backend, key));
    dialogs.push_back(new PluginNoIsolationDialog(env.backend, plugin));
    dialogs.push_back(new PluginRemoveDialog(env.backend, plugin));
    for (QDialog* each : dialogs)
    {
        each->show();
        settle();
        vss_test::scale_text(each, 2.0);
        settle();
        REQUIRE(vss_test::footer_fully_visible(*each));
        for (QPushButton* button : each->findChildren<QPushButton*>())
        {
            if (button->isVisibleTo(each))
                REQUIRE(button->width() >= button->sizeHint().width() - 1);
        }
        each->close();
        delete each;
    }
}

// -- layout of the page -------------------------------------------------------

namespace
{

/// The Row widget that holds the switch of this plugin.
QWidget* row_widget(PluginsDialog& dialog, const QString& id)
{
    Switch* box = dialog.row_switch(id);
    return box != nullptr ? box->parentWidget() : nullptr;
}

QPushButton* detail_button(PluginsDialog& dialog, const QString& text)
{
    for (QPushButton* button : dialog.detail()->findChildren<QPushButton*>())
    {
        if (button->text() == text)
            return button;
    }
    return nullptr;
}

int top_in_detail(PluginsDialog& dialog, const char* object_name)
{
    auto* widget =
        dialog.detail()->findChild<QWidget*>(QString::fromLatin1(object_name));
    REQUIRE(widget != nullptr);
    return widget->mapTo(dialog.detail(), QPoint(0, 0)).y();
}

} // namespace

TEST_CASE("plugins page: a row shows a state chip only for a state its switch "
          "cannot say")
{
    fixture env;
    PluginsDialog dialog(env.backend);
    dialog.show();
    settle();
    int with_chip = 0;
    int without_chip = 0;
    for (const plugin_view& each : env.backend.snapshot()->plugins)
    {
        StatusChip* chip = dialog.row_chip(each.id, each.folder);
        REQUIRE(chip != nullptr);
        const bool has_switch =
            dialog.row_switch(each.id, each.folder) != nullptr;
        const bool said_by_switch =
            has_switch && (each.state == plugin_state::available ||
                           each.state == plugin_state::off);
        REQUIRE_EQ(chip->isVisibleTo(chip->parentWidget()), !said_by_switch);
        (said_by_switch ? without_chip : with_chip) += 1;
        // Whether it shows or not, a screen reader still hears the state.
        const auto row = static_cast<int>(dialog.row_keys().indexOf(
            each.id + QLatin1Char('|') + each.folder));
        const QModelIndex index = dialog.list()->model()->index(row, 0);
        REQUIRE(index.data(Qt::AccessibleTextRole)
                    .toString()
                    .endsWith(state_text(each.state)));
    }
    // The samples have both kinds of row, and a copy that is not used, which
    // has no switch, keeps its chip.
    REQUIRE(with_chip > 0);
    REQUIRE(without_chip > 0);
    StatusChip* copy_chip = dialog.row_chip(
        QStringLiteral("com.virtualserialstudio.modbus"),
        QStringLiteral(
            "C:/Users/you/Studio/plugins/Modbus fixture (your copy)"));
    REQUIRE(copy_chip != nullptr);
    REQUIRE(copy_chip->isVisibleTo(copy_chip->parentWidget()));
    REQUIRE_EQ(copy_chip->text(), QStringLiteral("Not used"));
}

TEST_CASE("plugins page: a wide chip does not take room from the line under "
          "the name")
{
    fixture env;
    PluginsDialog dialog(env.backend);
    dialog.show();
    settle();
    // "Needs your decision" is the widest chip. Its row and a row with no chip
    // both show the word Off and the switch on their second line, so the
    // second line has the same room in both.
    QWidget* decision = row_widget(dialog, QStringLiteral("org.hobby.scanner"));
    QWidget* plain = row_widget(dialog, QStringLiteral("org.example.noise"));
    REQUIRE(decision != nullptr);
    REQUIRE(plain != nullptr);
    auto* decision_second =
        decision->findChild<QLabel*>(QStringLiteral("pluginRowSecond"));
    auto* plain_second =
        plain->findChild<QLabel*>(QStringLiteral("pluginRowSecond"));
    REQUIRE(decision_second != nullptr);
    REQUIRE(plain_second != nullptr);
    REQUIRE(dialog.row_chip(QStringLiteral("org.hobby.scanner"))
                ->isVisibleTo(decision));
    REQUIRE(!dialog.row_chip(QStringLiteral("org.example.noise"))
                 ->isVisibleTo(plain));
    REQUIRE_EQ(decision_second->width(), plain_second->width());
}

TEST_CASE("plugins page: the detail puts what a plugin may do before its "
          "technical facts")
{
    fixture env;
    PluginsDialog dialog(env.backend);
    dialog.resize(dialog.width(), 900);
    dialog.show();
    settle();
    dialog.select(QStringLiteral("com.virtualserialstudio.modbus"));
    settle();

    // What it is, who made it and what it may do, in that order, then where it
    // is used, then the technical facts, then the messages.
    const char* order[] = {
        "pluginDetailName",        "pluginDetailAdds",
        "pluginDetailDescription", "pluginDetailVersion",
        "pluginDetailAuthor",      "pluginDetailSignature",
        "pluginDetailMayDo",       "pluginDetailShowUse",
        "pluginDetailTechnical",   "pluginDetailFingerprint",
        "pluginDetailLicence",     "pluginDetailTiming",
        "pluginDetailIsolation",   "pluginDetailInstalledFor",
        "pluginDetailFolder",      "pluginDetailLog"};
    int last = -1;
    for (const char* name : order)
    {
        const int top = top_in_detail(dialog, name);
        REQUIRE(top > last);
        last = top;
    }
}

TEST_CASE("plugins page: the detail has no Plugin mark, and a plugin with no "
          "technical facts has no heading for them")
{
    fixture env;
    PluginsDialog dialog(env.backend);
    dialog.show();
    settle();
    for (const plugin_view& each : env.backend.snapshot()->plugins)
    {
        dialog.select(each.id, each.folder);
        settle();
        REQUIRE(dialog.detail()->findChild<QWidget*>(
                    QStringLiteral("pluginDetailMark")) == nullptr);
        // The heading is there exactly when a row of the second group is.
        const bool heading =
            dialog.detail()->findChild<QWidget*>(
                QStringLiteral("pluginDetailTechnical")) != nullptr;
        const bool facts =
            dialog.detail()->findChild<QWidget*>(
                QStringLiteral("pluginDetailFingerprint")) != nullptr ||
            dialog.detail()->findChild<QWidget*>(
                QStringLiteral("pluginDetailFolder")) != nullptr ||
            dialog.detail()->findChild<QWidget*>(
                QStringLiteral("pluginDetailTiming")) != nullptr;
        REQUIRE_EQ(heading, facts);
    }
}

TEST_CASE("plugins page: the next step is a normal button and the rest are "
          "quiet, and running without isolation is never the normal one")
{
    for (const plugin_action next :
         {plugin_action::start_now, plugin_action::review_and_decide,
          plugin_action::show_used_copy, plugin_action::licence,
          plugin_action::start_again})
        REQUIRE(is_next_step(next));
    for (const plugin_action other :
         {plugin_action::copy_details, plugin_action::run_without_isolation,
          plugin_action::show_log})
        REQUIRE(!is_next_step(other));

    fixture env;
    PluginsDialog dialog(env.backend);
    dialog.show();
    settle();
    const auto role_of = [&](const QString& id, const QString& text)
    {
        dialog.select(id);
        settle();
        QPushButton* button = detail_button(dialog, text);
        REQUIRE(button != nullptr);
        return button->property("role").toString();
    };
    REQUIRE_EQ(role_of(QStringLiteral("org.example.crashy"),
                       QStringLiteral("Start again")),
               QString());
    REQUIRE_EQ(role_of(QStringLiteral("org.example.crashy"),
                       QStringLiteral("Show log")),
               QStringLiteral("quiet"));
    REQUIRE_EQ(role_of(QStringLiteral("org.hobby.scanner"),
                       QStringLiteral("Review and decide")),
               QString());
    // A plugin that cannot be kept apart has two quiet actions, and neither
    // is drawn as the step to take.
    REQUIRE_EQ(role_of(QStringLiteral("org.example.isolate"),
                       QStringLiteral("Run without isolation...")),
               QStringLiteral("quiet"));
    REQUIRE_EQ(role_of(QStringLiteral("org.example.isolate"),
                       QStringLiteral("Copy details")),
               QStringLiteral("quiet"));
}

TEST_CASE("plugins page: Add a plugin is a button and the rest are in a menu "
          "under More options")
{
    fixture env;
    PluginsDialog dialog(env.backend);
    dialog.show();
    settle();
    REQUIRE(dialog.add_button()->isVisibleTo(&dialog));
    REQUIRE_EQ(dialog.add_button()->text(), QStringLiteral("Add a plugin..."));
    REQUIRE_EQ(dialog.more_button()->text(), QStringLiteral("More options"));
    QMenu* menu = dialog.more_button()->menu();
    REQUIRE(menu != nullptr);
    QStringList texts;
    for (const QAction* action : menu->actions())
        texts << action->text();
    REQUIRE_EQ(texts, (QStringList{QStringLiteral("Import a key..."),
                                   QStringLiteral("Open the plugin folder"),
                                   QStringLiteral("Check again")}));
    // None of the four is a link under the list any more.
    for (QPushButton* button : dialog.findChildren<QPushButton*>())
    {
        REQUIRE(button->text() != QStringLiteral("Import a key..."));
        REQUIRE(button->text() != QStringLiteral("Check again"));
    }
    // It is a menu button for assistive technology, with a name.
    QAccessibleInterface* interface =
        QAccessible::queryAccessibleInterface(dialog.more_button());
    REQUIRE(interface != nullptr);
    REQUIRE(interface->role() == QAccessible::ButtonMenu);
    REQUIRE_EQ(accessible_name(dialog.more_button()),
               QStringLiteral("More options"));

    // "Open the plugin folder" asks the file manager for the user's folder.
    QString opened;
    dialog.set_path_opener([&opened](const QString& path) { opened = path; });
    dialog.folder_action()->trigger();
    REQUIRE_EQ(opened, env.backend.snapshot()->user_folder);
}

TEST_CASE("plugins page: the badge follows the signature sentence, the "
          "timing is explained under its value and the folder's actions sit "
          "together under the path")
{
    fixture env;
    PluginsDialog dialog(env.backend);
    dialog.resize(dialog.width(), 900);
    dialog.show();
    settle();
    dialog.select(QStringLiteral("com.virtualserialstudio.modbus"));
    settle();

    auto* sentence = dialog.detail()->findChild<QLabel*>(
        QStringLiteral("pluginDetailSignatureText"));
    auto* badge = dialog.detail()->findChild<QWidget*>(
        QStringLiteral("pluginOfficialBadge"));
    REQUIRE(sentence != nullptr);
    REQUIRE(badge != nullptr);
    const int gap = badge->geometry().left() - sentence->geometry().right();
    REQUIRE(gap >= 0);
    REQUIRE(gap <= 24);

    auto* timing = dialog.detail()->findChild<QLabel*>(
        QStringLiteral("pluginDetailTimingText"));
    auto* timing_line = dialog.detail()->findChild<QLabel*>(
        QStringLiteral("pluginDetailTimingSentence"));
    REQUIRE(timing != nullptr);
    REQUIRE(timing_line != nullptr);
    REQUIRE_EQ(timing->text(), QStringLiteral("Timed by Studio"));
    REQUIRE_EQ(timing_line->text(),
               timing_sentence(env.sample(
                   QStringLiteral("com.virtualserialstudio.modbus"),
                   QStringLiteral("C:/Users/you/Studio/plugins/Modbus "
                                  "fixture"))));
    REQUIRE(timing_line->isVisibleTo(dialog.detail()));
    dialog.select(QStringLiteral("org.example.noise"));
    settle();
    timing_line = dialog.detail()->findChild<QLabel*>(
        QStringLiteral("pluginDetailTimingSentence"));
    REQUIRE(timing_line != nullptr);
    REQUIRE_EQ(timing_line->text(),
               QStringLiteral("Data on a link waits for this plugin, for up to "
                              "800 microseconds each time."));

    dialog.select(QStringLiteral("com.virtualserialstudio.modbus"));
    settle();
    auto* path = dialog.detail()->findChild<QWidget*>(
        QStringLiteral("pluginDetailFolderText"));
    auto* show = dialog.detail()->findChild<QWidget*>(
        QStringLiteral("pluginDetailShowFolder"));
    auto* copy = dialog.detail()->findChild<QWidget*>(
        QStringLiteral("pluginDetailFolderCopy"));
    REQUIRE(path != nullptr);
    REQUIRE(show != nullptr);
    REQUIRE(copy != nullptr);
    const int path_bottom =
        path->mapTo(dialog.detail(), QPoint(0, path->height())).y();
    const int show_middle =
        show->mapTo(dialog.detail(), QPoint(0, show->height() / 2)).y();
    const int copy_middle =
        copy->mapTo(dialog.detail(), QPoint(0, copy->height() / 2)).y();
    REQUIRE(show_middle >= path_bottom);
    REQUIRE(copy_middle >= path_bottom);
    REQUIRE(std::abs(show_middle - copy_middle) <= 4);
}

// -- pictures for a person to look at -----------------------------------------

TEST_CASE("plugins page: pictures for review, when a folder is named")
{
    // Set VSS_PLUGINS_GRAB_DIR to a folder outside the repository to have the
    // page and its dialogs drawn to PNG files, in both themes and at 200
    // percent text. Without it this test does nothing. The pictures are for a
    // person to look at; they are never committed.
    const QString folder =
        QString::fromLocal8Bit(qgetenv("VSS_PLUGINS_GRAB_DIR"));
    if (folder.isEmpty())
        return;
    QDir().mkpath(folder);
    for (const theme_mode mode : {theme_mode::light, theme_mode::dark})
    {
        Theme::instance().set_mode(mode);
        const QString theme = mode == theme_mode::light
                                  ? QStringLiteral("light")
                                  : QStringLiteral("dark");
        for (const double scale : {1.0, 2.0})
        {
            const QString suffix = QStringLiteral("%1-%2").arg(
                theme,
                scale > 1.0 ? QStringLiteral("200") : QStringLiteral("100"));
            fixture env;
            const auto save = [&](QWidget& widget, const QString& name)
            {
                if (scale > 1.0)
                {
                    vss_test::scale_text(&widget, scale);
                    settle();
                    // The window grows to what the larger text needs, as it
                    // does for a person.
                    widget.resize(
                        widget.minimumSizeHint().expandedTo(widget.size()));
                }
                settle();
                widget.grab().save(folder + QStringLiteral("/plugins-") + name +
                                   QLatin1Char('-') + suffix +
                                   QStringLiteral(".png"));
            };
            for (const char* id :
                 {"org.hobby.scanner", "org.example.crashy",
                  "com.virtualserialstudio.modbus", "org.example.isolate"})
            {
                PluginsDialog page(env.backend);
                page.resize(page.width(), 760);
                page.show();
                settle();
                page.select(QString::fromLatin1(id));
                settle();
                save(page,
                     QString::fromLatin1(id).section(QLatin1Char('.'), -1));
            }
            // The whole detail of two plugins at once, so the order of its
            // sections can be judged without scrolling.
            if (scale <= 1.0)
            {
                for (const char* id :
                     {"com.virtualserialstudio.modbus", "org.hobby.scanner"})
                {
                    PluginsDialog page(env.backend);
                    page.resize(page.width(), 1500);
                    page.show();
                    settle();
                    page.select(QString::fromLatin1(id));
                    settle();
                    save(page,
                         QString::fromLatin1(id).section(QLatin1Char('.'), -1) +
                             QStringLiteral("-tall"));
                }
            }
            PluginDecisionDialog decision(
                env.backend, env.sample(QStringLiteral("org.hobby.scanner")),
                PluginDecisionDialog::mode::decide);
            decision.show();
            save(decision, QStringLiteral("decision-unsigned"));
            PluginDecisionDialog changed(
                env.backend, env.sample(QStringLiteral("org.example.changed")),
                PluginDecisionDialog::mode::decide);
            changed.show();
            save(changed, QStringLiteral("decision-changed"));
            plugin_key_file_view key;
            key.author = QStringLiteral("Example Instruments");
            key.key_id = QStringLiteral("example-1");
            key.fingerprint = QStringLiteral("0123456789abcdef");
            key.inside_plugin_folder = true;
            key.path =
                QStringLiteral("C:/Users/you/plugins/tool/author.vsskey.json");
            PluginKeyImportDialog key_dialog(env.backend, key);
            key_dialog.show();
            save(key_dialog, QStringLiteral("key"));
            PluginNoIsolationDialog isolation(
                env.backend, env.sample(QStringLiteral("org.example.isolate")));
            isolation.show();
            save(isolation, QStringLiteral("isolation"));
        }
    }
    Theme::instance().set_mode(theme_mode::system);
}

TEST_CASE("plugins page: until the first scan has finished it says it is "
          "looking, not that there are none")
{
    FakePluginBackend backend;
    plugin_snapshot snapshot;
    snapshot.looking = true;
    backend.set_snapshot(snapshot);
    backend.flush();
    PluginsDialog dialog(backend);
    dialog.show();
    settle();
    auto* text = dialog.findChild<QLabel*>(QStringLiteral("pluginsEmptyText"));
    auto* title = dialog.findChild<QLabel*>(QStringLiteral("pluginsEmptyTitle"));
    REQUIRE(text != nullptr);
    REQUIRE_EQ(text->text(), QStringLiteral("Looking for plugins..."));
    REQUIRE(!title->isVisibleTo(&dialog));
    snapshot.looking = false;
    backend.set_snapshot(snapshot);
    backend.flush();
    settle();
    REQUIRE(title->isVisibleTo(&dialog));
    REQUIRE_EQ(title->text(), QStringLiteral("No plugins are installed"));
}

// -- what a plugin runs, and what it said of its licence
// ------------------------

namespace
{

plugin_view plugin_with_points()
{
    plugin_view view = fake_plugin_samples().front();
    view.id = QStringLiteral("org.example.points");
    view.name = QStringLiteral("Points");
    view.folder = QStringLiteral("C:/plugins/points");
    view.state = plugin_state::available;
    view.note = plugin_note::none;
    view.adds.clear();
    plugin_addition one;
    one.kind = plugin_addition_kind::generator;
    one.type_id = QStringLiteral("pulse");
    one.type_name = QStringLiteral("Pulse");
    one.max_instances = 2;
    plugin_addition two;
    two.kind = plugin_addition_kind::fixture;
    two.type_id = QStringLiteral("meter");
    two.type_name = QStringLiteral("Meter");
    two.max_instances = 1;
    plugin_addition three;
    three.kind = plugin_addition_kind::generator;
    three.type_id = QStringLiteral("tuned");
    three.type_name = QStringLiteral("Tuned");
    three.max_instances = 1;
    three.needs_settings = true;
    view.adds = {one, two, three};
    view.instances.clear();
    view.log.clear();
    return view;
}

} // namespace

TEST_CASE("plugins page: what it runs lists the instances and adds and removes")
{
    plugin_view plugin = plugin_with_points();
    plugin_instance_view running;
    running.number = 1;
    running.point_id = QStringLiteral("pulse");
    running.point_name = QStringLiteral("Pulse");
    running.state = plugin_instance_state::running;
    plugin_instance_view refused;
    refused.number = 2;
    refused.point_id = QStringLiteral("meter");
    refused.point_name = QStringLiteral("Meter");
    refused.state = plugin_instance_state::refused;
    refused.refusal = QStringLiteral("The plugin turned it down.");
    refused.reason = QStringLiteral("No such port.");
    plugin.instances = {running, refused};
    fixture env({plugin});
    PluginsDialog dialog(env.backend);
    dialog.show();
    settle();
    dialog.select(plugin.id);
    settle();
    QWidget* detail = dialog.detail();
    const auto states =
        detail->findChildren<QLabel*>(QStringLiteral("pluginInstanceState"));
    REQUIRE_EQ(states.size(), 2);
    REQUIRE_EQ(states[0]->text(), QStringLiteral("Running"));
    REQUIRE_EQ(states[1]->text(), QStringLiteral("Refused"));
    const auto says =
        detail->findChild<QLabel*>(QStringLiteral("pluginInstanceSays"));
    REQUIRE(says != nullptr);
    REQUIRE(says->text().contains(QStringLiteral("No such port.")));
    REQUIRE(says->textFormat() == Qt::PlainText);

    // Remove asks for the instance by its number.
    const auto buttons = detail->findChildren<QPushButton*>(
        QStringLiteral("pluginInstanceRemove"));
    REQUIRE_EQ(buttons.size(), 2);
    REQUIRE_EQ(buttons[0]->accessibleName(), QStringLiteral("Remove Pulse"));
    env.backend.clear_requests();
    buttons[0]->click();
    REQUIRE_EQ(env.backend.requests().size(), std::size_t{1});
    REQUIRE(env.backend.requests()[0].kind ==
            plugin_request_kind::remove_instance);
    REQUIRE_EQ(env.backend.requests()[0].number, quint64{1});
    env.backend.flush();
    settle();

    // Add offers what has room and needs no setting Studio cannot ask for.
    auto* add = dialog.detail()->findChild<QPushButton*>(
        QStringLiteral("pluginInstanceAdd"));
    REQUIRE(add != nullptr);
    REQUIRE(add->isEnabled());
    QMenu* menu = add->menu();
    REQUIRE(menu != nullptr);
    REQUIRE_EQ(menu->actions().size(), 3);
    REQUIRE_EQ(menu->actions()[0]->text(), QStringLiteral("Pulse"));
    REQUIRE(menu->actions()[0]->isEnabled());
    // Meter is the refused instance's point, with one allowed.
    REQUIRE(!menu->actions()[1]->isEnabled());
    // Tuned has a setting with no default.
    REQUIRE(!menu->actions()[2]->isEnabled());
    env.backend.clear_requests();
    menu->actions()[0]->trigger();
    REQUIRE_EQ(env.backend.requests().size(), std::size_t{1});
    REQUIRE(env.backend.requests()[0].kind ==
            plugin_request_kind::add_instance);
    REQUIRE_EQ(env.backend.requests()[0].path, QStringLiteral("pulse"));
    REQUIRE(env.backend.requests()[0].values.isEmpty());
}

TEST_CASE("plugins page: a point with settings to ask for asks first")
{
    plugin_view plugin = plugin_with_points();
    plugin_addition& point = plugin.adds.front();
    plugin_parameter_ask port;
    port.id = QStringLiteral("port");
    port.label = QStringLiteral("Port to write to");
    port.help = QStringLiteral("A port in your setup.");
    port.place_name = true;
    plugin_parameter_ask note;
    note.id = QStringLiteral("note");
    note.label = QStringLiteral("A note");
    point.asks = {port, note};
    fixture env({plugin});
    env.backend.set_place_names(
        {QStringLiteral("Alpha"), QStringLiteral("Beta")});
    PluginsDialog dialog(env.backend);
    dialog.show();
    settle();
    dialog.select(plugin.id);
    settle();
    auto* add = dialog.detail()->findChild<QPushButton*>(
        QStringLiteral("pluginInstanceAdd"));
    REQUIRE(add != nullptr);
    env.backend.clear_requests();
    add->menu()->actions()[0]->trigger();
    settle();
    // Nothing is made yet: the dialog asks first.
    REQUIRE(env.backend.requests().empty());
    PluginInstanceDialog* ask = nullptr;
    for (QWidget* each : QApplication::topLevelWidgets())
    {
        if (auto* found = qobject_cast<PluginInstanceDialog*>(each);
            found != nullptr && found->isVisible())
            ask = found;
    }
    REQUIRE(ask != nullptr);
    REQUIRE_EQ(ask->windowTitle(), QStringLiteral("Add Pulse"));
    REQUIRE(!ask->add_button()->isEnabled());
    auto* ports = qobject_cast<QComboBox*>(ask->field(QStringLiteral("port")));
    auto* text = qobject_cast<QLineEdit*>(ask->field(QStringLiteral("note")));
    REQUIRE(ports != nullptr);
    REQUIRE(text != nullptr);
    REQUIRE_EQ(ports->count(), 2);
    REQUIRE_EQ(ports->accessibleName(), QStringLiteral("Port to write to"));
    REQUIRE_EQ(ports->accessibleDescription(),
               QStringLiteral("A port in your setup."));
    ports->setCurrentText(QStringLiteral("Alpha"));
    REQUIRE(!ask->add_button()->isEnabled());
    text->setText(QStringLiteral("  hello  "));
    REQUIRE(ask->add_button()->isEnabled());
    ask->add_button()->click();
    settle();
    REQUIRE_EQ(env.backend.requests().size(), std::size_t{1});
    REQUIRE(env.backend.requests()[0].kind ==
            plugin_request_kind::add_instance);
    REQUIRE_EQ(env.backend.requests()[0].values.value(QStringLiteral("port")),
               QStringLiteral("Alpha"));
    REQUIRE_EQ(env.backend.requests()[0].values.value(QStringLiteral("note")),
               QStringLiteral("hello"));

    // Cancel makes nothing. The page was rebuilt when the instance was made.
    env.backend.clear_requests();
    settle();
    add = dialog.detail()->findChild<QPushButton*>(
        QStringLiteral("pluginInstanceAdd"));
    REQUIRE(add != nullptr);
    add->menu()->actions()[0]->trigger();
    settle();
    PluginInstanceDialog* again = nullptr;
    for (QWidget* each : QApplication::topLevelWidgets())
    {
        if (auto* found = qobject_cast<PluginInstanceDialog*>(each);
            found != nullptr && found->isVisible())
            again = found;
    }
    REQUIRE(again != nullptr);
    again->cancel_button()->click();
    settle();
    REQUIRE(env.backend.requests().empty());
}

TEST_CASE("plugins page: a plugin that is not running keeps its instances")
{
    plugin_view plugin = plugin_with_points();
    plugin.state = plugin_state::off;
    plugin_instance_view kept;
    kept.number = 1;
    kept.point_id = QStringLiteral("pulse");
    kept.point_name = QStringLiteral("Pulse");
    kept.state = plugin_instance_state::not_running;
    plugin.instances = {kept};
    fixture env({plugin});
    PluginsDialog dialog(env.backend);
    dialog.show();
    settle();
    dialog.select(plugin.id);
    settle();
    const auto states = dialog.detail()->findChildren<QLabel*>(
        QStringLiteral("pluginInstanceState"));
    REQUIRE_EQ(states.size(), 1);
    REQUIRE_EQ(states[0]->text(), QStringLiteral("Not running"));
    // A plugin that cannot be used cannot be given more to do.
    plugin_view broken = plugin_with_points();
    broken.state = plugin_state::cannot_be_used;
    broken.note = plugin_note::invalid_signature;
    env.backend.set_plugins({broken});
    env.backend.flush();
    settle();
    dialog.select(broken.id);
    settle();
    auto* add = dialog.detail()->findChild<QPushButton*>(
        QStringLiteral("pluginInstanceAdd"));
    REQUIRE(add != nullptr);
    REQUIRE(!add->isEnabled());
}

TEST_CASE("plugins page: what the plugin said of its licence, and licence.read")
{
    plugin_view plugin = plugin_with_points();
    plugin.trust = plugin_trust::official;
    plugin.has_licence_report = true;
    plugin.licence_report_reason = vss::license::reason::expired;
    plugin.licence_status = vss::license::no_licence();
    fixture env({plugin});
    PluginsDialog dialog(env.backend);
    dialog.show();
    settle();
    dialog.select(plugin.id);
    settle();
    auto* report = dialog.detail()->findChild<NoteBanner*>(
        QStringLiteral("pluginDetailLicenceReport"));
    REQUIRE(report != nullptr);
    // It is near the top, above the facts and what the plugin may do, where a
    // disagreement is seen.
    {
        auto* adds = dialog.detail()->findChild<QWidget*>(
            QStringLiteral("pluginDetailAdds"));
        REQUIRE(adds != nullptr);
        REQUIRE(report->mapTo(dialog.detail(), QPoint(0, 0)).y() <
                adds->mapTo(dialog.detail(), QPoint(0, 0)).y());
    }
    const QString sentence =
        reason_sentence(vss::license::reason::expired, plugin.licence_status);
    REQUIRE(!sentence.isEmpty());
    // Studio's own check found nothing wrong, so the two disagree.
    REQUIRE_EQ(report->text(),
               QStringLiteral("This plugin did not accept the licence: ") +
                   sentence +
                   QStringLiteral(" Studio and the plugin disagree. Check that "
                                  "Studio and the plugin are both up to "
                                  "date."));
    // Studio's check agrees: no second sentence.
    plugin.licence_reason = vss::license::reason::expired;
    env.backend.set_plugins({plugin});
    env.backend.flush();
    settle();
    dialog.select(plugin.id);
    settle();
    report = dialog.detail()->findChild<NoteBanner*>(
        QStringLiteral("pluginDetailLicenceReport"));
    REQUIRE(report != nullptr);
    REQUIRE_EQ(report->text(),
               QStringLiteral("This plugin did not accept the licence: ") +
                   sentence);
    // `ok` shows nothing.
    plugin.licence_report_reason = vss::license::reason::ok;
    env.backend.set_plugins({plugin});
    env.backend.flush();
    settle();
    dialog.select(plugin.id);
    settle();
    REQUIRE(dialog.detail()->findChild<NoteBanner*>(
                QStringLiteral("pluginDetailLicenceReport")) == nullptr);

    // The permission sentence says what the file holds.
    plugin_permission_view licence_read;
    licence_read.id = QStringLiteral("licence.read");
    REQUIRE_EQ(permission_sentence(licence_read),
               QStringLiteral("Read your licence file. The file holds your "
                              "name."));
    REQUIRE(!is_amber_permission(QStringLiteral("licence.read")));
}

TEST_CASE(
    "plugins page: what a decoder found is a plain table that follows the "
    "store")
{
    plugin_view plugin = plugin_with_points();
    plugin_instance_view decoder;
    decoder.number = 3;
    decoder.point_id = QStringLiteral("pulse");
    decoder.point_name = QStringLiteral("Pulse");
    decoder.state = plugin_instance_state::running;
    decoder.decoder = true;
    plugin.instances = {decoder};
    fixture env({plugin});
    PluginsDialog dialog(env.backend);
    dialog.show();
    settle();
    dialog.select(plugin.id);
    settle();
    QWidget* detail = dialog.detail();
    // Nothing yet.
    auto* empty =
        detail->findChild<QLabel*>(QStringLiteral("pluginFoundEmpty"));
    REQUIRE(empty != nullptr);
    REQUIRE_EQ(empty->text(), QStringLiteral("Nothing found yet."));

    auto what = std::make_shared<plugin_found>();
    plugin_found_row frame;
    frame.time_ns = 12'345'000'000;
    frame.place = QStringLiteral("Alpha");
    frame.received = true;
    frame.what = QStringLiteral("<b>hello</b>") + QChar(0x202e);
    frame.kind = QStringLiteral("greeting");
    plugin_found_row trigger;
    trigger.time_ns = 13'000'000'000;
    trigger.place = QStringLiteral("Alpha");
    trigger.received = false;
    trigger.trigger = true;
    trigger.what = QStringLiteral("no end byte");
    trigger.kind = QStringLiteral("Framing");
    what->rows = {trigger, frame};
    what->held = 5;
    what->dropped = 2;
    what->refused = 1;
    what->revision = 7;
    env.backend.set_found(plugin.id, 3, what);
    // The table follows on the timer.
    REQUIRE(vss_test::wait_until(
        [&]
        {
            auto* table = detail->findChild<QTableWidget*>(
                QStringLiteral("pluginFoundTable"));
            return table != nullptr && table->rowCount() == 2;
        },
        5000));
    auto* table =
        detail->findChild<QTableWidget*>(QStringLiteral("pluginFoundTable"));
    REQUIRE_EQ(table->accessibleName(), QStringLiteral("What Pulse found"));
    REQUIRE_EQ(table->columnCount(), 5);
    REQUIRE_EQ(table->item(0, 0)->text(), QStringLiteral("13.000 s"));
    REQUIRE_EQ(table->item(0, 2)->text(), QStringLiteral("Sent"));
    REQUIRE_EQ(table->item(0, 3)->text(), QStringLiteral("no end byte"));
    REQUIRE_EQ(table->item(1, 0)->text(), QStringLiteral("12.345 s"));
    REQUIRE_EQ(table->item(1, 2)->text(), QStringLiteral("Received"));
    // The plugin's words are text, with the direction mark gone.
    REQUIRE_EQ(table->item(1, 3)->text(), QStringLiteral("<b>hello</b>"));
    REQUIRE(table->editTriggers() == QAbstractItemView::NoEditTriggers);
    auto* counts =
        detail->findChild<QLabel*>(QStringLiteral("pluginFoundCounts"));
    REQUIRE(counts != nullptr);
    REQUIRE(counts->isVisibleTo(detail));
    REQUIRE(counts->text().contains(QStringLiteral("2 dropped")));
    REQUIRE(counts->text().contains(QStringLiteral("1 refused")));
    REQUIRE(
        counts->text().contains(QStringLiteral("Showing the newest 2 of 5.")));
}

TEST_CASE("plugins page: an invalid description file says what is wrong")
{
    plugin_view plugin;
    plugin.id = QStringLiteral("org.example.invalid");
    plugin.state = plugin_state::cannot_be_used;
    plugin.note = plugin_note::invalid_manifest;

    // Without a line from the host, only the general sentence.
    REQUIRE_EQ(note_for(plugin).text,
               QStringLiteral("Its description file is not valid, so Studio "
                              "cannot tell what it is."));

    plugin.detail = QStringLiteral("an official plugin must have not_after");
    REQUIRE_EQ(note_for(plugin).text,
               QStringLiteral("Its description file is not valid, so Studio "
                              "cannot tell what it is. What is wrong: an "
                              "official plugin must have not_after"));
}

TEST_CASE("decoded traffic: frames and triggers of every decoder, newest "
          "first, as plain text, with a filter for triggers")
{
    plugin_view plugin = plugin_with_points();
    plugin_instance_view decoder;
    decoder.number = 3;
    decoder.point_id = QStringLiteral("pulse");
    decoder.point_name = QStringLiteral("Pulse");
    decoder.state = plugin_instance_state::running;
    decoder.decoder = true;
    plugin.instances = {decoder};
    fixture env({plugin});

    DecodedDialog dialog(env.backend);
    dialog.show();
    settle();
    // Nothing found yet: the window says so and how to start.
    REQUIRE_EQ(dialog.row_count(), 0);
    REQUIRE_EQ(dialog.empty_label()->text(),
               QStringLiteral("Nothing found yet."));

    auto what = std::make_shared<plugin_found>();
    plugin_found_row frame;
    frame.time_ns = 12'345'000'000;
    frame.place = QStringLiteral("Alpha");
    frame.received = true;
    frame.what = QStringLiteral("<b>hello</b>");
    frame.kind = QStringLiteral("greeting");
    plugin_found_row trigger;
    trigger.time_ns = 13'000'000'000;
    trigger.place = QStringLiteral("Alpha");
    trigger.received = false;
    trigger.trigger = true;
    trigger.what = QStringLiteral("no end byte");
    trigger.kind = QStringLiteral("Framing");
    what->rows = {frame, trigger};
    what->revision = 1;
    env.backend.set_found(plugin.id, 3, what);
    dialog.refresh();

    // Newest first, with the type, and the plugin's words as they are.
    REQUIRE_EQ(dialog.row_count(), 2);
    REQUIRE_EQ(dialog.cell(0, 3), QStringLiteral("Trigger"));
    REQUIRE_EQ(dialog.cell(0, 2), QStringLiteral("Sent"));
    REQUIRE_EQ(dialog.cell(0, 5), QStringLiteral("no end byte"));
    REQUIRE_EQ(dialog.cell(1, 3), QStringLiteral("Frame"));
    REQUIRE_EQ(dialog.cell(1, 5), QStringLiteral("<b>hello</b>"));

    dialog.triggers_only_check()->setChecked(true);
    REQUIRE_EQ(dialog.row_count(), 1);
    REQUIRE_EQ(dialog.cell(0, 3), QStringLiteral("Trigger"));

    // With no decoder at all, it says how to start one.
    plugin.instances.clear();
    env.backend.set_plugins({plugin});
    env.backend.flush();
    dialog.triggers_only_check()->setChecked(false);
    dialog.refresh();
    REQUIRE_EQ(dialog.row_count(), 0);
    REQUIRE(dialog.empty_label()->text().contains(
        QStringLiteral("Decode as...")));
}
