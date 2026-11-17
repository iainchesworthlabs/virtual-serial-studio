//
// Copyright (c) 2026 Virtual Serial Studio contributors
//
// SPDX-License-Identifier: GPL-3.0-or-later
//

#include "support/about_dialog.hpp"

#include "support/build_info.hpp"
#include "support/licence_text.hpp"
#include "support/third_party.hpp"
#include "support/update_text.hpp"
#include "theme/icon_provider.hpp"
#include "theme/theme.hpp"
#include "updates/update_service.hpp"
#include "widgets/common.hpp"
#include "widgets/controls.hpp"
#include "widgets/dialogs.hpp"

#include <vss/core/profile.hpp>

#include <QAccessible>
#include <QCheckBox>
#include <QClipboard>
#include <QCoreApplication>
#include <QDesktopServices>
#include <QFontDatabase>
#include <QFrame>
#include <QGridLayout>
#include <QGuiApplication>
#include <QHBoxLayout>
#include <QLabel>
#include <QPushButton>
#include <QScrollArea>
#include <QSignalBlocker>
#include <QTabWidget>
#include <QTimer>
#include <QUrl>
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

/// Tells assistive technology about a change, politely: it waits for the
/// person to finish what they were hearing.
void announce_politely(QWidget* widget, const QString& text)
{
    if (widget == nullptr || text.isEmpty())
        return;
    QAccessibleAnnouncementEvent event(widget, text);
    event.setPoliteness(QAccessible::AnnouncementPoliteness::Polite);
    QAccessible::updateAccessibility(&event);
}

/// The label of a row in the card of a definition list.
QLabel* row_label(const QString& text, QWidget* parent)
{
    auto* label = new QLabel(text, parent);
    label->setFont(Theme::instance().font(text_style::caption));
    label->setProperty("muted", true);
    label->setContentsMargins(14, 8, 14, 8);
    label->setAlignment(Qt::AlignLeft | Qt::AlignTop);
    polish(label);
    return label;
}

/// The value of a row. A row whose value is empty is left out.
QLabel* row_value(QWidget* parent)
{
    auto* value = new QLabel(parent);
    value->setFont(Theme::instance().font(text_style::body));
    value->setContentsMargins(14, 8, 14, 8);
    value->setTextInteractionFlags(Qt::TextSelectableByMouse);
    value->setWordWrap(true);
    return value;
}

} // namespace

AboutDialog::AboutDialog(StudioBackend& backend, QWidget* parent)
    : AboutDialog(backend, nullptr, parent)
{
}

AboutDialog::AboutDialog(StudioBackend& backend,
                         updates::UpdateService* updates, QWidget* parent)
    : QDialog(parent), info_(current_build_info()),
      edition_(edition_label(backend.licence().status)), updates_(updates)
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

    // The application icon is a picture in its own colours (the accent tile is
    // part of it), so it is drawn as it is in both themes.
    auto* tile = new QLabel(header);
    tile->setFixedSize(56, 56);
    tile->setAlignment(Qt::AlignCenter);
    tile->setPixmap(IconProvider::instance().app_icon().pixmap(
        QSize(56, 56), tile->devicePixelRatioF()));
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
            "version 3. Edition: %2.")
            .arg(info_.full_version(), edition_),
        header);
    sub->setFont(Theme::instance().font(text_style::body));
    sub->setProperty("muted", true);
    sub->setWordWrap(true);
    polish(sub);
    heading->addWidget(sub);
    header_layout->addLayout(heading, 1);
    outer->addWidget(header);

    // Five tabs follow, in this order: About, Build information,
    // Environment, Licences, Fonts. With the update service, Updates comes
    // second.
    // page() makes each tab's content page and its vertical layout.
    tabs_ = new QTabWidget(this);
    tabs_->setObjectName(QStringLiteral("aboutTabs"));
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
            "published by the Free Software Foundation, version 3 of the "
            "License."),
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
    // The link to the source, which the licence asks us to offer. It opens in
    // the system's browser when the user follows it.
    auto* source = new QLabel(
        QCoreApplication::translate("About", "Source code: %1")
            .arg(QStringLiteral("<a href=\"%1\">%1</a>").arg(source_url())),
        about_page);
    source->setFont(Theme::instance().font(text_style::body));
    source->setTextFormat(Qt::RichText);
    source->setWordWrap(true);
    source->setTextInteractionFlags(Qt::TextBrowserInteraction);
    connect(source, &QLabel::linkActivated, this,
            [](const QString& url) { QDesktopServices::openUrl(QUrl(url)); });
    about_layout->addWidget(source);
    about_layout->addStretch(1);
    tabs_->addTab(about_page, QCoreApplication::translate("About", "About"));

    // Updates tab, second: only with the update service.
    if (updates_ != nullptr)
    {
        updates_page_ = build_updates_page();
        tabs_->addTab(updates_page_,
                      QCoreApplication::translate("About", "Updates"));
    }

    // Build information tab, then Environment tab.
    // Each is a label and value grid built from the same info_ as its copied
    // report, with its own copy button. Values can be selected with the mouse.
    tabs_->addTab(
        rows_page(
            QCoreApplication::translate(
                "About",
                "Copy this into a bug report. It shows which build you are "
                "running."),
            build_information_rows(info_),
            QCoreApplication::translate("About", "Copy build information"),
            &AboutDialog::copy_build_information),
        QCoreApplication::translate("About", "Build information"));
    tabs_->addTab(
        rows_page(
            QCoreApplication::translate(
                "About",
                "Copy this into a bug report. It answers \"what is this "
                "running on?\""),
            environment_rows(info_),
            QCoreApplication::translate("About", "Copy environment"),
            &AboutDialog::copy_environment),
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
            "Public License, version 3. It uses these components:"),
        licences_page));
    for (const third_party_component& component :
         bundled_components(profiling_compiled()))
        add_licence(licences_layout, component.name,
                    licence_text(component.spdx),
                    licence_note(component.notice), licences_page);
    licences_layout->addStretch(1);
    tabs_->addTab(licences_page,
                  QCoreApplication::translate("About", "Licences"));

    // Fonts tab.
    // The families third_party.json lists as bundled, each with its licence.
    // With none, it says so and reports the families the system supplies for
    // the interface and for fixed-width text.
    QVBoxLayout* fonts_layout = nullptr;
    QWidget* fonts_page = page(tabs_, fonts_layout);
    const QVector<third_party_font> fonts = third_party_fonts();
    if (fonts.isEmpty())
    {
        fonts_layout->addWidget(body_label(
            QCoreApplication::translate(
                "About",
                "No fonts are bundled. Studio uses your system's UI font."),
            fonts_page));
        const QFont ui = QFontDatabase::systemFont(QFontDatabase::GeneralFont);
        const QFont fixed =
            QFontDatabase::systemFont(QFontDatabase::FixedFont);
        fonts_layout->addWidget(body_label(
            QCoreApplication::translate("About", "Interface font: %1.")
                .arg(ui.family()),
            fonts_page));
        fonts_layout->addWidget(body_label(
            QCoreApplication::translate("About", "Fixed font: %1.")
                .arg(fixed.family()),
            fonts_page));
    }
    for (const third_party_font& font : fonts)
        add_licence(fonts_layout,
                    font.version.isEmpty()
                        ? font.family
                        : QStringLiteral("%1 %2").arg(font.family, font.version),
                    licence_text(font.spdx), font.url, fonts_page);
    fonts_layout->addStretch(1);
    tabs_->addTab(fonts_page, QCoreApplication::translate("About", "Fonts"));

    outer->addWidget(tabs_, 1);

    // Footer: the bundle request is only signalled here (the caller opens
    // the bundle dialog), and Close is the one default button.
    QHBoxLayout* footer_layout = nullptr;
    QFrame* footer = make_dialog_footer(this, footer_layout);
    auto* bundle = make_dialog_button(
        QCoreApplication::translate("About", "Create support bundle"), "quiet",
        footer);
    bundle->setAutoDefault(false);
    connect(bundle, &QPushButton::clicked, this,
            &AboutDialog::bundle_requested);
    footer_layout->addWidget(bundle);
    auto* copy = make_dialog_button(
        QCoreApplication::translate("About", "Copy diagnostic information"),
        nullptr, footer);
    copy->setAutoDefault(false);
    connect(copy, &QPushButton::clicked, this,
            &AboutDialog::copy_diagnostic_information);
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

void AboutDialog::show_updates_tab()
{
    if (updates_page_ != nullptr)
        tabs_->setCurrentWidget(updates_page_);
}

QWidget* AboutDialog::build_updates_page()
{
    QVBoxLayout* layout = nullptr;
    QWidget* result = page(tabs_, layout);
    QWidget* content = static_cast<QScrollArea*>(result)->widget();

    // The definition list, in a card. Rows with no value are left out.
    auto* card = new QFrame(content);
    card->setObjectName(QStringLiteral("updatesCard"));
    card->setProperty("card", true);
    polish(card);
    auto* grid = new QGridLayout(card);
    grid->setContentsMargins(0, 0, 0, 0);
    grid->setHorizontalSpacing(0);
    grid->setVerticalSpacing(0);
    // The "Plugins" row of the spec goes here, after "Last checked": one
    // line for each plugin with an update. The service has no plugin data
    // yet, so there is no such row.
    updates_version_label_ =
        row_label(QCoreApplication::translate("About", "This version"), card);
    updates_version_ = row_value(card);
    updates_version_->setObjectName(QStringLiteral("updatesVersion"));
    auto* newest_label =
        row_label(QCoreApplication::translate("About", "Newest version"), card);
    updates_status_ = row_value(card);
    updates_status_->setObjectName(QStringLiteral("updatesStatus"));
    updates_checked_label_ =
        row_label(QCoreApplication::translate("About", "Last checked"), card);
    updates_checked_ = row_value(card);
    updates_checked_->setObjectName(QStringLiteral("updatesChecked"));
    grid->addWidget(updates_version_label_, 0, 0);
    grid->addWidget(updates_version_, 0, 1);
    grid->addWidget(newest_label, 1, 0);
    grid->addWidget(updates_status_, 1, 1);
    grid->addWidget(updates_checked_label_, 2, 0);
    grid->addWidget(updates_checked_, 2, 1);
    grid->setColumnStretch(1, 1);
    layout->addWidget(card);

    // The same setting as in Settings.
    updates_automatic_ = new QCheckBox(
        QCoreApplication::translate("About", "Check for updates automatically"),
        content);
    updates_automatic_->setObjectName(QStringLiteral("updatesAutomatic"));
    layout->addWidget(updates_automatic_);
    auto* note = new QLabel(
        QCoreApplication::translate(
            "About",
            "Once a day Studio asks virtual-serial-studio.com for the newest "
            "version. Nothing about you or this computer is sent."),
        content);
    note->setFont(Theme::instance().font(text_style::caption));
    note->setProperty("muted", true);
    note->setWordWrap(true);
    polish(note);
    layout->addWidget(note);
    layout->addStretch(1);

    // The buttons of the tab, right aligned. The dialog's own footer is
    // unchanged.
    auto* buttons = new QHBoxLayout();
    buttons->setContentsMargins(0, 0, 0, 0);
    buttons->setSpacing(8);
    buttons->addStretch(1);
    updates_check_ = make_dialog_button(
        QCoreApplication::translate("About", "Check now"), nullptr, content);
    updates_check_->setObjectName(QStringLiteral("updatesCheck"));
    updates_check_->setAutoDefault(false);
    buttons->addWidget(updates_check_);
    updates_update_ = make_dialog_button(
        QCoreApplication::translate("About", "Update..."), "primary", content);
    updates_update_->setObjectName(QStringLiteral("updatesUpdate"));
    updates_update_->setAutoDefault(false);
    buttons->addWidget(updates_update_);
    layout->addLayout(buttons);

    connect(updates_automatic_, &QCheckBox::toggled, this,
            [this](bool on) { updates_->set_automatic(on); });
    connect(updates_check_, &QPushButton::clicked, this,
            [this] { check_for_updates(); });
    connect(updates_update_, &QPushButton::clicked, this,
            [this] { Q_EMIT update_requested(); });
    connect(updates_, &updates::UpdateService::changed, this,
            [this] { refresh_updates(); });
    refresh_updates();
    return result;
}

void AboutDialog::check_for_updates()
{
    // Pressing the button disables it, which moves the focus away. Remember
    // that it had it, so it can have it back when the check ends.
    updates_restore_focus_ = focusWidget() == updates_check_;
    updates_->check_now();
}

void AboutDialog::refresh_updates()
{
    const updates::snapshot& state = updates_->state();

    const QString version =
        update_this_version_text(info_.version, state.install_kind);
    updates_version_->setText(version);
    updates_version_->setAccessibleName(version);
    updates_version_->setVisible(!version.isEmpty());
    updates_version_label_->setVisible(!version.isEmpty());

    // The sentence is a polite live region: it is announced when it changes,
    // without moving focus.
    const QString sentence = update_status_text(state, info_.version);
    const bool sentence_changed = updates_status_->text() != sentence;
    updates_status_->setText(sentence);
    updates_status_->setAccessibleName(sentence);
    if (sentence_changed && isVisible())
        announce_politely(updates_status_, sentence);

    const QString checked = update_last_checked_text(state.last_checked);
    updates_checked_->setText(checked);
    updates_checked_->setAccessibleName(checked);
    updates_checked_->setVisible(!checked.isEmpty());
    updates_checked_label_->setVisible(!checked.isEmpty());

    {
        const QSignalBlocker block(updates_automatic_);
        updates_automatic_->setChecked(updates_->automatic());
    }
    updates_automatic_->setEnabled(updates_->available());

    const bool checking = state.state == updates::check_state::checking;
    updates_check_->setEnabled(updates_->available() && !checking);
    updates_update_->setVisible(state.state == updates::check_state::available);
    // A check that ended: the button has the focus again if it had it.
    if (updates_was_checking_ && !checking && updates_restore_focus_)
    {
        updates_restore_focus_ = false;
        if (updates_check_->isEnabled())
            updates_check_->setFocus(Qt::OtherFocusReason);
    }
    updates_was_checking_ = checking;
}

QString AboutDialog::source_url()
{
    return QStringLiteral(
        "https://github.com/iainchesworthlabs/virtual-serial-studio");
}

QString AboutDialog::build_information_text() const
{
    return build_information_report(info_);
}

QString AboutDialog::environment_text() const
{
    return environment_report(info_);
}

QString AboutDialog::diagnostic_text() const
{
    return diagnostic_report(info_);
}

void AboutDialog::copy_build_information()
{
    QGuiApplication::clipboard()->setText(build_information_text());
    ToastManager::instance().show(
        this,
        QCoreApplication::translate("About", "Build information copied."));
}

void AboutDialog::copy_environment()
{
    QGuiApplication::clipboard()->setText(environment_text());
    ToastManager::instance().show(
        this, QCoreApplication::translate("About", "Environment copied."));
}

void AboutDialog::copy_diagnostic_information()
{
    QGuiApplication::clipboard()->setText(diagnostic_text());
    ToastManager::instance().show(
        this, QCoreApplication::translate("About",
                                          "Diagnostic information copied."));
}

QWidget* AboutDialog::rows_page(const QString& intro,
                                const QVector<environment_row>& rows,
                                const QString& copy_text,
                                void (AboutDialog::*copy)())
{
    QVBoxLayout* layout = nullptr;
    QWidget* result = page(tabs_, layout);
    QWidget* content = static_cast<QScrollArea*>(result)->widget();
    layout->addWidget(body_label(intro, content));
    auto* card = new QFrame(content);
    card->setProperty("card", true);
    polish(card);
    auto* grid = new QGridLayout(card);
    grid->setContentsMargins(0, 0, 0, 0);
    grid->setHorizontalSpacing(0);
    grid->setVerticalSpacing(0);
    for (int index = 0; index < rows.size(); ++index)
    {
        auto* label = new QLabel(
            QCoreApplication::translate(
                "About", rows.at(index).label.toUtf8().constData()),
            card);
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
    layout->addWidget(card);
    auto* button = make_dialog_button(copy_text, nullptr, content);
    button->setAutoDefault(false);
    connect(button, &QPushButton::clicked, this, copy);
    layout->addWidget(button, 0, Qt::AlignLeft);
    layout->addStretch(1);
    return result;
}

} // namespace vss::gui::support
