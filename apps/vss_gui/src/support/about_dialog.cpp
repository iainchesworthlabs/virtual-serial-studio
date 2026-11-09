//
// Copyright (c) 2026 Virtual Serial Studio contributors
//
// SPDX-License-Identifier: GPL-3.0-or-later
//

#include "support/about_dialog.hpp"

#include "support/build_info.hpp"
#include "theme/icon_provider.hpp"
#include "theme/theme.hpp"
#include "widgets/common.hpp"
#include "widgets/controls.hpp"
#include "widgets/dialogs.hpp"

#include <vss/core/profile.hpp>

#include <QClipboard>
#include <QCoreApplication>
#include <QFontDatabase>
#include <QFrame>
#include <QGridLayout>
#include <QGuiApplication>
#include <QHBoxLayout>
#include <QLabel>
#include <QPushButton>
#include <QScrollArea>
#include <QTabWidget>
#include <QTimer>
#include <QVBoxLayout>

namespace vss::gui::support
{
namespace
{

QFont semibold(QFont font)
{
    font.setWeight(QFont::DemiBold);
    return font;
}

QLabel* body_label(const QString& text, QWidget* parent)
{
    auto* label = new QLabel(text, parent);
    label->setFont(Theme::instance().font(text_style::body));
    label->setWordWrap(true);
    label->setTextInteractionFlags(Qt::TextSelectableByMouse);
    return label;
}

QWidget* page(QWidget* parent, QVBoxLayout*& layout)
{
    auto* content = new QWidget(parent);
    auto* content_layout = new QVBoxLayout(content);
    content_layout->setContentsMargins(24, 18, 24, 18);
    content_layout->setSpacing(12);
    layout = content_layout;
    auto* scroll = new QScrollArea(parent);
    scroll->setFrameShape(QFrame::NoFrame);
    scroll->setWidgetResizable(true);
    scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    scroll->setWidget(content);
    return scroll;
}

void add_licence(QVBoxLayout* layout, const QString& name,
                 const QString& licence, const QString& note, QWidget* parent)
{
    auto* heading = new QLabel(name, parent);
    heading->setFont(semibold(Theme::instance().font(text_style::body)));
    layout->addWidget(heading);
    auto* terms = new QLabel(licence, parent);
    terms->setFont(Theme::instance().font(text_style::body));
    terms->setWordWrap(true);
    terms->setTextInteractionFlags(Qt::TextSelectableByMouse);
    layout->addWidget(terms);
    if (!note.isEmpty())
    {
        auto* detail = new QLabel(note, parent);
        detail->setFont(Theme::instance().font(text_style::caption));
        detail->setProperty("muted", true);
        detail->setWordWrap(true);
        polish(detail);
        layout->addWidget(detail);
    }
}

} // namespace

AboutDialog::AboutDialog(QWidget* parent)
    : QDialog(parent), info_(current_build_info())
{
    build();
}

void AboutDialog::build()
{
    VSS_PROFILE_ZONE("gui.support.about.build");
    VSS_PROFILE_MESSAGE("gui.dialog.about.opened");
    // Window chrome: accessible name, panel surface and the standard dialog
    // width, which is also the minimum.
    setWindowTitle(
        QCoreApplication::translate("About", "About Virtual Serial Studio"));
    setAccessibleName(
        QCoreApplication::translate("About", "About Virtual Serial Studio"));
    setProperty("surface", QStringLiteral("panel"));
    polish(this);
    resize(Theme::instance().metric(
               QStringLiteral("layout.dialogWidths.standard")),
           620);
    setMinimumWidth(Theme::instance().metric(
        QStringLiteral("layout.dialogWidths.standard")));

    // The outer layout has no margins so the header, tabs and footer can set
    // their own padding. The header is an app icon tile beside the name and
    // version.
    auto* outer = new QVBoxLayout(this);
    outer->setContentsMargins(0, 0, 0, 0);
    outer->setSpacing(0);

    auto* header = new QWidget(this);
    auto* header_layout = new QHBoxLayout(header);
    header_layout->setContentsMargins(24, 20, 24, 14);
    header_layout->setSpacing(14);

    auto* tile = new QLabel(header);
    tile->setFixedSize(56, 56);
    tile->setAlignment(Qt::AlignCenter);
    const auto apply_tile_style = [tile]
    {
        tile->setStyleSheet(
            QStringLiteral("QLabel { background: %1; border-radius: %2px; }")
                .arg(Theme::instance()
                         .colour(colour_role::accent)
                         .name(QColor::HexRgb))
                .arg(Theme::instance().radius(QStringLiteral("card"))));
        tile->setPixmap(
            IconProvider::instance()
                .icon(QStringLiteral("device"), colour_role::onAccent, 30)
                .pixmap(30, 30));
    };
    // The tile colours and icon come from the theme, so apply them now and
    // again whenever the theme changes. The lambda copies the tile pointer;
    // the connection ends with the tile as its receiver.
    apply_tile_style();
    connect(&Theme::instance(), &Theme::theme_changed, tile,
            [apply_tile_style] { apply_tile_style(); });
    header_layout->addWidget(tile, 0, Qt::AlignTop);

    // Name on the first line, version and licence summary below it.
    auto* heading = new QVBoxLayout();
    heading->setContentsMargins(0, 0, 0, 0);
    heading->setSpacing(4);
    auto* title = new QLabel(
        QCoreApplication::translate("About", "Virtual Serial Studio"), header);
    title->setFont(Theme::instance().font(text_style::title));
    title->setWordWrap(true);
    heading->addWidget(title);
    auto* sub = new QLabel(
        QCoreApplication::translate(
            "About",
            "Version %1. Free software under the GNU General Public License, "
            "version 3 or later.")
            .arg(info_.version),
        header);
    sub->setFont(Theme::instance().font(text_style::body));
    sub->setProperty("muted", true);
    sub->setWordWrap(true);
    polish(sub);
    heading->addWidget(sub);
    header_layout->addLayout(heading, 1);
    outer->addWidget(header);

    // Four tabs follow, in this order: About, Environment, Licences, Fonts.
    // page() makes each tab's content page and its vertical layout.
    tabs_ = new QTabWidget(this);
    tabs_->setAccessibleName(
        QCoreApplication::translate("About", "About details"));
    tabs_->setDocumentMode(true);

    // About tab.
    // The standard GPL notice, split into its three paragraphs.
    QVBoxLayout* about_layout = nullptr;
    QWidget* about_page = page(tabs_, about_layout);
    about_layout->addWidget(body_label(
        QCoreApplication::translate(
            "About",
            "This program is free software: you can redistribute it and/or "
            "modify it under the terms of the GNU General Public License as "
            "published by the Free Software Foundation, either version 3 of "
            "the "
            "License, or (at your option) any later version."),
        about_page));
    about_layout->addWidget(body_label(
        QCoreApplication::translate(
            "About",
            "This program is distributed in the hope that it will be useful, "
            "but WITHOUT ANY WARRANTY; without even the implied warranty of "
            "MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU "
            "General Public License for more details."),
        about_page));
    about_layout->addWidget(body_label(
        QCoreApplication::translate(
            "About",
            "You should have received a copy of the GNU General Public License "
            "along with this program. If not, see "
            "https://www.gnu.org/licenses/."),
        about_page));
    about_layout->addStretch(1);
    tabs_->addTab(about_page, QCoreApplication::translate("About", "About"));

    // Environment tab.
    // The build information shown as a label and value grid, built from the
    // same info_ as the copied report. Values can be selected with the mouse.
    QVBoxLayout* environment_layout = nullptr;
    QWidget* environment_page = page(tabs_, environment_layout);
    environment_layout->addWidget(body_label(
        QCoreApplication::translate(
            "About",
            "Copy this into a bug report. It answers \"which build is this?\""),
        environment_page));
    auto* card = new QFrame(environment_page);
    card->setProperty("card", true);
    polish(card);
    auto* grid = new QGridLayout(card);
    grid->setContentsMargins(0, 0, 0, 0);
    grid->setHorizontalSpacing(0);
    grid->setVerticalSpacing(0);
    const QVector<environment_row> rows = environment_rows(info_);
    for (int index = 0; index < rows.size(); ++index)
    {
        auto* label = new QLabel(rows.at(index).label, card);
        label->setFont(Theme::instance().font(text_style::caption));
        label->setProperty("muted", true);
        label->setContentsMargins(14, 8, 14, 8);
        polish(label);
        auto* value = new QLabel(rows.at(index).value, card);
        value->setFont(Theme::instance().font(text_style::data));
        value->setContentsMargins(14, 8, 14, 8);
        value->setTextInteractionFlags(Qt::TextSelectableByMouse);
        value->setWordWrap(true);
        grid->addWidget(label, index, 0);
        grid->addWidget(value, index, 1);
    }
    // Let the value column take the spare width so long values wrap there.
    grid->setColumnStretch(1, 1);
    environment_layout->addWidget(card);
    environment_layout->addStretch(1);
    tabs_->addTab(environment_page,
                  QCoreApplication::translate("About", "Environment"));

    // Licences tab.
    // One entry per component, each with its licence and an optional note on
    // how Studio uses it.
    QVBoxLayout* licences_layout = nullptr;
    QWidget* licences_page = page(tabs_, licences_layout);
    licences_layout->addWidget(body_label(
        QCoreApplication::translate(
            "About",
            "Virtual Serial Studio is free software under the GNU General "
            "Public License, version 3 or later. It uses these components:"),
        licences_page));
    add_licence(licences_layout, QCoreApplication::translate("About", "Qt 6"),
                QCoreApplication::translate(
                    "About",
                    "GNU Lesser General Public License, version 3, or the GNU "
                    "General Public License, version 3."),
                QCoreApplication::translate(
                    "About", "Linked dynamically as installed on this PC."),
                licences_page);
    add_licence(licences_layout, QCoreApplication::translate("About", "Boost"),
                QCoreApplication::translate(
                    "About", "Boost Software License, version 1.0."),
                {}, licences_page);
    add_licence(licences_layout,
                QCoreApplication::translate("About", "Boost.VirtualSerial"),
                QCoreApplication::translate(
                    "About", "Boost Software License, version 1.0."),
                {}, licences_page);
    add_licence(
        licences_layout, QCoreApplication::translate("About", "Tracy"),
        QCoreApplication::translate("About", "BSD 3-Clause License."),
        QCoreApplication::translate(
            "About", "Only present when profiling support is built in."),
        licences_page);
    add_licence(
        licences_layout, QCoreApplication::translate("About", "com0com"),
        QCoreApplication::translate("About",
                                    "GNU General Public License, version 3."),
        QCoreApplication::translate(
            "About",
            "An optional driver for Windows, installed separately by you."),
        licences_page);
    licences_layout->addStretch(1);
    tabs_->addTab(licences_page,
                  QCoreApplication::translate("About", "Licences"));

    // Fonts tab.
    // Studio bundles none, so report the families the system supplies for the
    // interface and for fixed-width text.
    QVBoxLayout* fonts_layout = nullptr;
    QWidget* fonts_page = page(tabs_, fonts_layout);
    fonts_layout->addWidget(body_label(
        QCoreApplication::translate(
            "About",
            "No fonts are bundled. Studio uses your system's UI font."),
        fonts_page));
    const QFont ui = QFontDatabase::systemFont(QFontDatabase::GeneralFont);
    const QFont fixed = QFontDatabase::systemFont(QFontDatabase::FixedFont);
    fonts_layout->addWidget(
        body_label(QCoreApplication::translate("About", "Interface font: %1.")
                       .arg(ui.family()),
                   fonts_page));
    fonts_layout->addWidget(
        body_label(QCoreApplication::translate("About", "Fixed font: %1.")
                       .arg(fixed.family()),
                   fonts_page));
    fonts_layout->addStretch(1);
    tabs_->addTab(fonts_page, QCoreApplication::translate("About", "Fonts"));

    outer->addWidget(tabs_, 1);

    // Footer: the bundle request is only signalled here (the caller opens
    // the bundle dialog), and Close is the one default button.
    QHBoxLayout* footer_layout = nullptr;
    QFrame* footer = make_dialog_footer(this, footer_layout);
    auto* bundle = make_dialog_button(
        QCoreApplication::translate("About", "Create diagnostic bundle"),
        "quiet", footer);
    bundle->setAutoDefault(false);
    connect(bundle, &QPushButton::clicked, this,
            &AboutDialog::bundle_requested);
    footer_layout->addWidget(bundle);
    auto* copy = make_dialog_button(
        QCoreApplication::translate("About", "Copy environment"), nullptr,
        footer);
    copy->setAutoDefault(false);
    connect(copy, &QPushButton::clicked, this, &AboutDialog::copy_environment);
    footer_layout->addWidget(copy);
    auto* close = make_dialog_button(
        QCoreApplication::translate("About", "Close"), "primary", footer);
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

QString AboutDialog::environment_text() const
{
    return environment_report(info_);
}

void AboutDialog::copy_environment()
{
    QGuiApplication::clipboard()->setText(environment_text());
    ToastManager::instance().show(
        this, QCoreApplication::translate("About", "Environment copied."));
}

} // namespace vss::gui::support
