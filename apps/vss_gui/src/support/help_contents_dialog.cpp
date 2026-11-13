//
// Copyright (c) 2026 Virtual Serial Studio contributors
//
// SPDX-License-Identifier: GPL-3.0-or-later
//

#include "support/help_contents_dialog.hpp"

#include "theme/theme.hpp"
#include "widgets/common.hpp"
#include "widgets/dialogs.hpp"

#include <vss/core/profile.hpp>

#include <QCoreApplication>
#include <QFrame>
#include <QHBoxLayout>
#include <QLabel>
#include <QPushButton>
#include <QTimer>
#include <QVBoxLayout>

namespace vss::gui::support
{
namespace
{

QString t(const char* text)
{
    return QCoreApplication::translate("Help", text);
}

} // namespace

HelpContentsDialog::HelpContentsDialog(QWidget* parent) : QDialog(parent)
{
    build();
}

void HelpContentsDialog::build()
{
    VSS_PROFILE_ZONE("gui.support.help.build");
    VSS_PROFILE_MESSAGE("gui.dialog.help.opened");
    // Window chrome: accessible name, panel surface and the small dialog
    // width, which is also the minimum.
    setWindowTitle(QCoreApplication::translate("Help", "Help contents"));
    setAccessibleName(QCoreApplication::translate("Help", "Help contents"));
    setProperty("surface", QStringLiteral("panel"));
    polish(this);
    resize(
        Theme::instance().metric(QStringLiteral("layout.dialogWidths.small")),
        460);
    setMinimumWidth(
        Theme::instance().metric(QStringLiteral("layout.dialogWidths.small")));

    // The outer layout has no margins so the header, body and footer can set
    // their own padding. Header first: the dialog heading.
    auto* outer = new QVBoxLayout(this);
    outer->setContentsMargins(0, 0, 0, 0);
    outer->setSpacing(0);

    auto* header = new QWidget(this);
    auto* header_layout = new QVBoxLayout(header);
    header_layout->setContentsMargins(24, 20, 24, 14);
    header_layout->setSpacing(4);
    add_dialog_heading(
        header_layout, QCoreApplication::translate("Help", "Help contents"),
        QCoreApplication::translate(
            "Help", "These are the most used actions. All actions are in the "
                    "menus."),
        header);
    outer->addWidget(header);

    // Body: one card per help topic.
    auto* body = new QWidget(this);
    auto* body_layout = new QVBoxLayout(body);
    body_layout->setContentsMargins(24, 4, 24, 16);
    body_layout->setSpacing(8);

    // Titles and descriptions are marked for translation here and translated
    // by t() when the card is built.
    struct topic
    {
        help_topic value{};
        const char* title = nullptr;
        const char* description = nullptr;
    };
    const topic topics[] = {
        {help_topic::setup_check, QT_TRANSLATE_NOOP("Help", "Setup check"),
         QT_TRANSLATE_NOOP(
             "Help",
             "Check the driver, permissions and devices this PC needs.")},
        {help_topic::troubleshoot,
         QT_TRANSLATE_NOOP("Help", "Why is nothing arriving?"),
         QT_TRANSLATE_NOOP(
             "Help",
             "Follow the five steps between the device and your program.")},
        {help_topic::shortcuts, QT_TRANSLATE_NOOP("Help", "Keyboard shortcuts"),
         QT_TRANSLATE_NOOP("Help", "See every action's shortcut in one list.")},
        {help_topic::about,
         QT_TRANSLATE_NOOP("Help", "About Virtual Serial Studio"),
         QT_TRANSLATE_NOOP("Help",
                           "Version, environment, licences and fonts.")},
    };

    for (const topic& item : topics)
    {
        // Each card is a title and a muted description on the left and an
        // Open button on the right.
        auto* card = new QFrame(body);
        card->setProperty("card", true);
        polish(card);
        auto* card_layout = new QHBoxLayout(card);
        card_layout->setContentsMargins(12, 10, 12, 10);
        card_layout->setSpacing(12);
        auto* column = new QVBoxLayout();
        column->setContentsMargins(0, 0, 0, 0);
        column->setSpacing(2);
        auto* title = new QLabel(t(item.title), card);
        QFont font = Theme::instance().font(text_style::body);
        font.setWeight(QFont::DemiBold);
        title->setFont(font);
        column->addWidget(title);
        auto* description = new QLabel(t(item.description), card);
        description->setFont(Theme::instance().font(text_style::caption));
        description->setProperty("muted", true);
        description->setWordWrap(true);
        polish(description);
        column->addWidget(description);
        card_layout->addLayout(column, 1);
        auto* open = make_dialog_button(
            QCoreApplication::translate("Help", "Open"), nullptr, card);
        // Close is the default button, not Open. The topic goes in the
        // accessible name so a screen reader can tell the Open buttons apart.
        open->setAutoDefault(false);
        open->setAccessibleName(tr("%1: %2").arg(
            QCoreApplication::translate("Help", "Open"), t(item.title)));
        // Capture the topic by value: item is gone once the loop ends.
        connect(open, &QPushButton::clicked, this,
                [this, value = item.value] { choose(value); });
        card_layout->addWidget(open, 0, Qt::AlignVCenter);
        body_layout->addWidget(card);
    }
    // The stretch keeps the cards at the top when the dialog is resized
    // taller.
    body_layout->addStretch(1);
    outer->addWidget(body, 1);

    // Footer with a single Close button, which accepts the dialog without
    // choosing a topic.
    QHBoxLayout* footer_layout = nullptr;
    QFrame* footer = make_dialog_footer(this, footer_layout);
    auto* close = make_dialog_button(
        QCoreApplication::translate("Help", "Close"), "primary", footer);
    close->setDefault(true);
    connect(close, &QPushButton::clicked, this, &QDialog::accept);
    footer_layout->addWidget(close);
    outer->addWidget(footer);

    // Focus Close now and again once the event loop runs, in case showing
    // the dialog moves focus elsewhere first.
    close->setFocus(Qt::OtherFocusReason);
    QTimer::singleShot(0, close,
                       [close] { close->setFocus(Qt::OtherFocusReason); });
}

void HelpContentsDialog::choose(help_topic topic)
{
    VSS_PROFILE_ZONE("gui.support.help.choose");
    requested_ = topic;
    accept();
}

} // namespace vss::gui::support
