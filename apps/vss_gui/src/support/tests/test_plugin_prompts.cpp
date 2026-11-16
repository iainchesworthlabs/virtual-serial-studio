//
// Copyright (c) 2026 Virtual Serial Studio contributors
//
// SPDX-License-Identifier: GPL-3.0-or-later
//
// The two dialogs a running plugin can ask for, and the queue that shows them
// (docs/gui-design/docs/screens/10-plugins.md): a code to read once, and a
// control session to allow or refuse. The fake plugin backend records the
// answers.
//

#include "qt_input.hpp"
#include "qt_wait.hpp"
#include "test_framework.hpp"

#include "backend/fake_plugin_backend.hpp"
#include "support/plugin_dialogs.hpp"
#include "support/plugin_prompts.hpp"
#include "widgets/notes.hpp"

#include <QApplication>
#include <QClipboard>
#include <QFrame>
#include <QGuiApplication>
#include <QKeyEvent>
#include <QLabel>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QWidget>

using namespace vss::gui;
using namespace vss::gui::support;

namespace
{

void settle()
{
    for (int round = 0; round < 4; ++round)
        QApplication::processEvents();
}

void key_click(QWidget* widget, Qt::Key key)
{
    QKeyEvent press(QEvent::KeyPress, key, Qt::NoModifier);
    QApplication::sendEvent(widget, &press);
    QKeyEvent release(QEvent::KeyRelease, key, Qt::NoModifier);
    QApplication::sendEvent(widget, &release);
}

plugin_secret_request secret_of(quint64 id, const QString& text = {})
{
    plugin_secret_request request;
    request.id = id;
    request.plugin_id = QStringLiteral("org.example.pair");
    request.plugin_name = QStringLiteral("Pairing helper");
    request.label = QStringLiteral("Pairing code");
    request.text = text.isEmpty() ? QStringLiteral("483 920") : text;
    request.seconds = 125;
    return request;
}

plugin_control_request control_of(quint64 id)
{
    plugin_control_request request;
    request.id = id;
    request.plugin_id = QStringLiteral("org.example.remote");
    request.plugin_name = QStringLiteral("Remote helper");
    request.peer = QStringLiteral("Alex at the lab");
    request.reason = QStringLiteral("To run tonight's test.");
    request.scopes = {QStringLiteral("monitor"), QStringLiteral("monitor.data"),
                      QStringLiteral("control.data"), QStringLiteral("monitor"),
                      QStringLiteral("odd.scope")};
    request.life_seconds = 24 * 3600;
    return request;
}

} // namespace

TEST_CASE("plugin secret dialog: what it shows, the countdown and Copy")
{
    PluginSecretDialog dialog(secret_of(1), 125);
    dialog.show();
    settle();
    REQUIRE(dialog.was_shown());
    REQUIRE_EQ(dialog.windowTitle(), QStringLiteral("Code from a plugin"));
    bool named = false;
    for (QLabel* each : dialog.findChildren<QLabel*>())
    {
        if (each->text() ==
            QStringLiteral("Pairing helper asks you to read this."))
            named = true;
    }
    REQUIRE(named);
    REQUIRE_EQ(
        dialog.findChild<QLabel*>(QStringLiteral("secretPluginId"))->text(),
        QStringLiteral("org.example.pair"));
    REQUIRE_EQ(dialog.code_box()->toPlainText(), QStringLiteral("483 920"));
    REQUIRE(dialog.code_box()->isReadOnly());
    REQUIRE_EQ(dialog.code_box()->accessibleName(),
               QStringLiteral("Pairing code"));
    REQUIRE_EQ(dialog.label_text()->text(), QStringLiteral("Pairing code"));
    REQUIRE(dialog.label_text()->textFormat() == Qt::PlainText);
    REQUIRE_EQ(dialog.time_line()->text(),
               QStringLiteral("Good for 2 min 5 s."));
    REQUIRE(dialog.close_button()->isDefault());
    REQUIRE(dialog.close_button()->hasFocus());

    dialog.tick();
    REQUIRE_EQ(dialog.time_line()->text(),
               QStringLiteral("Good for 2 min 4 s."));

    // Copy puts the code on the clipboard, and says so.
    QGuiApplication::clipboard()->clear();
    dialog.copy_button()->click();
    REQUIRE_EQ(QGuiApplication::clipboard()->text(), QStringLiteral("483 920"));
    REQUIRE_EQ(dialog.copy_button()->text(), QStringLiteral("Copied"));
    REQUIRE(dialog.seen());
    REQUIRE_EQ(dialog.code_box()->accessibleName(),
               QStringLiteral("Pairing code"));
    REQUIRE_EQ(dialog.code_box()->accessibleDescription(),
               QStringLiteral("4 8 3   9 2 0"));

    // The time runs out: the code is gone, and the dialog says so.
    bool timed_out = false;
    QObject::connect(&dialog, &PluginSecretDialog::timed_out_now, &dialog,
                     [&timed_out] { timed_out = true; });
    for (int second = 0; second < 124; ++second)
        dialog.tick();
    REQUIRE(timed_out);
    REQUIRE(dialog.timed_out());
    REQUIRE(dialog.code_box()->toPlainText().isEmpty());
    REQUIRE(!dialog.code_box()->isVisibleTo(&dialog));
    REQUIRE(!dialog.copy_button()->isEnabled());
    REQUIRE_EQ(
        dialog.time_line()->text(),
        QStringLiteral("This code is no longer good. Ask the plugin for a "
                       "new one."));
}

TEST_CASE("plugin secret dialog: a plugin's words are plain text")
{
    plugin_secret_request request =
        secret_of(2, QStringLiteral("<b>ABC</b> <a href=\"x\">go</a>"));
    request.label = QStringLiteral("<i>Code</i>") + QChar(0x202e);
    request.plugin_name = QStringLiteral("<u>Helper</u>");
    PluginSecretDialog dialog(request, 30);
    dialog.show();
    settle();
    // The code is shown exactly as it came, as text.
    REQUIRE_EQ(dialog.code_box()->toPlainText(),
               QStringLiteral("<b>ABC</b> <a href=\"x\">go</a>"));
    REQUIRE(dialog.label_text()->textFormat() == Qt::PlainText);
    REQUIRE(!dialog.label_text()->text().contains(QChar(0x202e)));
    REQUIRE(
        dialog.label_text()->text().contains(QStringLiteral("<i>Code</i>")));
    bool named = false;
    for (QLabel* each : dialog.findChildren<QLabel*>())
    {
        if (each->text().contains(QStringLiteral("<u>Helper</u>")))
            named = true;
    }
    REQUIRE(named);
}

TEST_CASE("plugin secret dialog: what counts as having seen the code")
{
    // Closed while the code was there: seen.
    PluginSecretDialog closed(secret_of(4), 30);
    closed.show();
    settle();
    closed.close_button()->click();
    REQUIRE(closed.seen());

    // Escape is Close.
    PluginSecretDialog escaped(secret_of(5), 30);
    escaped.show();
    settle();
    key_click(&escaped, Qt::Key_Escape);
    settle();
    REQUIRE(escaped.seen());

    // The time ran out before the person did anything: not seen, and
    // closing afterwards does not change that.
    PluginSecretDialog late(secret_of(6), 2);
    late.show();
    settle();
    late.tick();
    late.tick();
    REQUIRE(late.timed_out());
    late.close_button()->click();
    REQUIRE(!late.seen());

    // The program closing is not seeing it.
    PluginSecretDialog abandoned(secret_of(7), 30);
    abandoned.show();
    settle();
    abandoned.abandon();
    abandoned.reject();
    REQUIRE(!abandoned.seen());
}

TEST_CASE("plugin secret dialog: it works at 200 percent text")
{
    PluginSecretDialog dialog(secret_of(3, QString(120, QLatin1Char('A'))), 60);
    dialog.show();
    settle();
    vss_test::scale_text(&dialog, 2.0);
    settle();
    REQUIRE(vss_test::footer_fully_visible(dialog));
    for (QPushButton* button : dialog.findChildren<QPushButton*>())
    {
        REQUIRE(button->width() >= button->sizeHint().width() - 1);
        REQUIRE(button->height() >= 24);
    }
}

TEST_CASE("plugin control dialog: what it says and how it is answered")
{
    PluginControlDialog dialog(control_of(7));
    dialog.show();
    settle();
    REQUIRE_EQ(dialog.windowTitle(), QStringLiteral("Allow remote control?"));
    REQUIRE(dialog.refuse_button()->isDefault());
    REQUIRE(dialog.refuse_button()->hasFocus());
    REQUIRE_EQ(dialog.refuse_button()->text(), QStringLiteral("Refuse"));
    REQUIRE_EQ(dialog.allow_button()->text(), QStringLiteral("Allow"));
    // A scope that comes twice is shown once, and an unknown one is named.
    const QStringList sentences = dialog.scope_sentences();
    REQUIRE_EQ(sentences.size(), 4);
    REQUIRE_EQ(sentences[0],
               QStringLiteral("See the ports, links and their state"));
    REQUIRE_EQ(sentences[1],
               QStringLiteral("See the bytes that pass through the ports"));
    REQUIRE_EQ(sentences[2], QStringLiteral("Send bytes into the ports"));
    REQUIRE_EQ(sentences[3],
               QStringLiteral("odd.scope. Studio does not describe this one."));
    bool asks = false;
    for (QLabel* each : dialog.findChildren<QLabel*>())
    {
        if (each->text() ==
            QStringLiteral("Remote helper asks to use this computer's ports "
                           "for another computer."))
            asks = true;
    }
    REQUIRE(asks);
    auto* who = dialog.findChild<QLabel*>(QStringLiteral("controlWho"));
    auto* why = dialog.findChild<QLabel*>(QStringLiteral("controlWhy"));
    REQUIRE(who != nullptr);
    REQUIRE(why != nullptr);
    REQUIRE_EQ(
        who->text(),
        QStringLiteral("It says the other computer is: Alex at the lab"));
    REQUIRE(why->text().contains(QStringLiteral("To run tonight's test.")));
    REQUIRE(who->textFormat() == Qt::PlainText);
    REQUIRE(dialog.data_note() != nullptr);
    REQUIRE_EQ(dialog.data_note()->text(),
               QStringLiteral("The other computer can read or write what your "
                              "ports carry."));
    REQUIRE_EQ(dialog.life_line()->text(),
               QStringLiteral("This lasts 24 hour(s), or until the plugin "
                              "stops. You are asked again each time."));
    // Rows that let the other side see or send data carry a description.
    int amber = 0;
    for (QFrame* row :
         dialog.findChildren<QFrame*>(QStringLiteral("controlScope")))
    {
        if (!row->accessibleDescription().isEmpty())
            ++amber;
    }
    REQUIRE_EQ(amber, 3);

    // Enter answers Refuse.
    key_click(&dialog, Qt::Key_Return);
    settle();
    REQUIRE(dialog.result() == QDialog::Rejected);

    // Allow is a click on Allow.
    PluginControlDialog allowed(control_of(8));
    allowed.show();
    allowed.allow_button()->click();
    REQUIRE(allowed.result() == QDialog::Accepted);

    // Nothing said: it says so, and there is no box of the plugin's words.
    plugin_control_request bare = control_of(9);
    bare.peer.clear();
    bare.reason.clear();
    bare.scopes = {QStringLiteral("monitor")};
    bare.life_seconds = 90 * 60;
    PluginControlDialog quiet(bare);
    REQUIRE_EQ(quiet.findChild<QLabel*>(QStringLiteral("controlWho"))->text(),
               QStringLiteral("It did not say which computer."));
    REQUIRE(quiet.findChild<QLabel*>(QStringLiteral("controlWhy")) == nullptr);
    REQUIRE(quiet.data_note() == nullptr);
    REQUIRE_EQ(quiet.life_line()->text(),
               QStringLiteral("This lasts 90 minute(s), or until the plugin "
                              "stops. You are asked again each time."));
}

TEST_CASE("plugin control dialog: it works at 200 percent text")
{
    PluginControlDialog dialog(control_of(10));
    dialog.show();
    settle();
    vss_test::scale_text(&dialog, 2.0);
    settle();
    REQUIRE(vss_test::footer_fully_visible(dialog));
    for (QPushButton* button : dialog.findChildren<QPushButton*>())
        REQUIRE(button->width() >= button->sizeHint().width() - 1);
}

TEST_CASE("plugin prompts: a code is shown, then answered when it is closed")
{
    FakePluginBackend backend;
    QWidget window;
    window.show();
    PluginPromptQueue queue(backend, &window);
    backend.ask_secret(secret_of(1));
    settle();
    auto* shown = qobject_cast<PluginSecretDialog*>(queue.current_dialog());
    REQUIRE(shown != nullptr);
    REQUIRE(shown->was_shown());
    REQUIRE(backend.secret_answers().empty());
    // The queue does not keep the code: it is in the dialog only.
    REQUIRE_EQ(shown->code_box()->toPlainText(), QStringLiteral("483 920"));
    shown->close_button()->click();
    settle();
    REQUIRE_EQ(backend.secret_answers().size(), std::size_t{1});
    REQUIRE_EQ(backend.secret_answers()[0].first, quint64{1});
    REQUIRE(backend.secret_answers()[0].second);
    REQUIRE(queue.current_dialog() == nullptr);
}

TEST_CASE("plugin prompts: a second request waits for the first")
{
    FakePluginBackend backend;
    QWidget window;
    window.show();
    PluginPromptQueue queue(backend, &window);
    backend.ask_secret(secret_of(1));
    backend.ask_control(control_of(2));
    backend.ask_secret(secret_of(3));
    settle();
    REQUIRE(qobject_cast<PluginSecretDialog*>(queue.current_dialog()) !=
            nullptr);
    REQUIRE_EQ(queue.waiting(), std::size_t{2});
    REQUIRE(backend.secret_answers().empty());
    REQUIRE(backend.control_answers().empty());

    // The first is closed; the control request comes next, and is refused by
    // closing it.
    qobject_cast<PluginSecretDialog*>(queue.current_dialog())->reject();
    settle();
    REQUIRE_EQ(backend.secret_answers().size(), std::size_t{1});
    auto* control = qobject_cast<PluginControlDialog*>(queue.current_dialog());
    REQUIRE(control != nullptr);
    control->reject();
    settle();
    REQUIRE_EQ(backend.control_answers().size(), std::size_t{1});
    REQUIRE_EQ(backend.control_answers()[0].first, quint64{2});
    REQUIRE(!backend.control_answers()[0].second);

    // The third is next. Allow answers yes.
    auto* last = qobject_cast<PluginSecretDialog*>(queue.current_dialog());
    REQUIRE(last != nullptr);
    last->close_button()->click();
    settle();
    REQUIRE_EQ(backend.secret_answers().size(), std::size_t{2});
    REQUIRE_EQ(queue.waiting(), std::size_t{0});
}

TEST_CASE("plugin prompts: Allow answers yes and Refuse answers no")
{
    FakePluginBackend backend;
    QWidget window;
    window.show();
    PluginPromptQueue queue(backend, &window);
    backend.ask_control(control_of(1));
    settle();
    auto* first = qobject_cast<PluginControlDialog*>(queue.current_dialog());
    REQUIRE(first != nullptr);
    first->allow_button()->click();
    settle();
    backend.ask_control(control_of(2));
    settle();
    auto* second = qobject_cast<PluginControlDialog*>(queue.current_dialog());
    REQUIRE(second != nullptr);
    second->refuse_button()->click();
    settle();
    REQUIRE_EQ(backend.control_answers().size(), std::size_t{2});
    REQUIRE(backend.control_answers()[0].second);
    REQUIRE(!backend.control_answers()[1].second);
}

TEST_CASE("plugin prompts: a request that waited too long is not shown")
{
    FakePluginBackend backend;
    QWidget window;
    window.show();
    PluginPromptQueue queue(backend, &window);
    // A code with no time left is never shown.
    plugin_secret_request spent = secret_of(1);
    spent.seconds = 0;
    backend.ask_secret(spent);
    settle();
    REQUIRE(queue.current_dialog() == nullptr);
    REQUIRE_EQ(backend.secret_answers().size(), std::size_t{1});
    REQUIRE(!backend.secret_answers()[0].second);
    // A control request the plugin stopped waiting for is refused unseen.
    queue.set_control_patience(0);
    backend.ask_control(control_of(2));
    settle();
    REQUIRE(queue.current_dialog() == nullptr);
    REQUIRE_EQ(backend.control_answers().size(), std::size_t{1});
    REQUIRE(!backend.control_answers()[0].second);
}

TEST_CASE("plugin prompts: closing answers everything and does not wait")
{
    FakePluginBackend backend;
    QWidget window;
    window.show();
    PluginPromptQueue queue(backend, &window);
    backend.ask_secret(secret_of(1));
    backend.ask_control(control_of(2));
    backend.ask_secret(secret_of(3));
    settle();
    REQUIRE(queue.current_dialog() != nullptr);
    queue.shut_down();
    settle();
    // Nothing is counted as seen when the program closes.
    REQUIRE_EQ(backend.secret_answers().size(), std::size_t{2});
    REQUIRE(!backend.secret_answers()[0].second);
    REQUIRE(!backend.secret_answers()[1].second);
    REQUIRE_EQ(backend.control_answers().size(), std::size_t{1});
    REQUIRE(!backend.control_answers()[0].second);
    REQUIRE(queue.current_dialog() == nullptr);

    // A request that comes after is answered no at once.
    backend.ask_control(control_of(4));
    settle();
    REQUIRE_EQ(backend.control_answers().size(), std::size_t{2});
    REQUIRE(!backend.control_answers()[1].second);
}
