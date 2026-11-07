//
// Copyright (c) 2026 Virtual Serial Studio contributors
//
// SPDX-License-Identifier: GPL-3.0-or-later
//

#include "shell/launcher.hpp"

#include "backend/driver_error.hpp"
#include "shell/format.hpp"
#include "support/setup_facts.hpp"
#include "theme/focus_ring_style.hpp"
#include "theme/icon_provider.hpp"

#include <vss/studio/studio.hpp>

#include <QFileInfo>
#include <QFrame>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QKeyEvent>
#include <QLabel>
#include <QMouseEvent>
#include <QPainter>
#include <QPushButton>
#include <QSettings>
#include <QStringList>
#include <QVBoxLayout>

namespace vss::gui
{
using namespace support;

namespace
{

QFont semibold(QFont font)
{
    font.setWeight(QFont::DemiBold);
    return font;
}

} // namespace

LauncherCard::LauncherCard(const QString& icon, const QString& title,
                           const QString& description, const QString& result,
                           QWidget* parent)
    : QFrame(parent), title_(title)
{
    setProperty("card", true);
    polish(this);
    setFocusPolicy(Qt::StrongFocus);
    setCursor(Qt::PointingHandCursor);
    setAccessibleName(title);
    setToolTip(result);
    setMinimumHeight(112);
    setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);

    auto* layout = new QHBoxLayout(this);
    layout->setContentsMargins(20, 18, 20, 18);
    layout->setSpacing(16);

    auto* tile = new QFrame(this);
    tile->setFixedSize(44, 44);
    tile->setStyleSheet(QStringLiteral("QFrame { background: %1; border: 1px "
                                       "solid %2; border-radius: 6px; }")
                            .arg(Theme::instance()
                                     .colour(colour_role::accentSoft)
                                     .name(QColor::HexRgb),
                                 Theme::instance()
                                     .colour(colour_role::accentLine)
                                     .name(QColor::HexRgb)));
    auto* tile_layout = new QVBoxLayout(tile);
    tile_layout->setContentsMargins(0, 0, 0, 0);
    auto* glyph = new QLabel(tile);
    glyph->setAlignment(Qt::AlignCenter);
    glyph->setPixmap(IconProvider::instance()
                         .icon(icon, colour_role::accentStrong, 22)
                         .pixmap(22, 22));
    tile_layout->addWidget(glyph, 0, Qt::AlignCenter);
    layout->addWidget(tile, 0, Qt::AlignTop);

    auto* column = new QVBoxLayout();
    column->setContentsMargins(0, 0, 0, 0);
    column->setSpacing(4);
    auto* heading = new QLabel(title, this);
    heading->setFont(semibold(Theme::instance().font(text_style::bodyLarge)));
    heading->setWordWrap(true);
    auto* body = new QLabel(description, this);
    body->setFont(Theme::instance().font(text_style::body));
    body->setWordWrap(true);
    auto* result_label = new QLabel(result, this);
    result_label->setFont(Theme::instance().font(text_style::caption));
    result_label->setProperty("muted", true);
    result_label->setWordWrap(true);
    column->addWidget(heading);
    column->addWidget(body);
    column->addWidget(result_label);
    layout->addLayout(column, 1);

    connect(&Theme::instance(), &Theme::theme_changed, this,
            [this, tile, glyph, icon]
            {
                tile->setStyleSheet(
                    QStringLiteral("QFrame { background: %1; border: 1px solid "
                                   "%2; border-radius: 6px; }")
                        .arg(Theme::instance()
                                 .colour(colour_role::accentSoft)
                                 .name(QColor::HexRgb),
                             Theme::instance()
                                 .colour(colour_role::accentLine)
                                 .name(QColor::HexRgb)));
                glyph->setPixmap(IconProvider::instance()
                                     .icon(icon, colour_role::accentStrong, 22)
                                     .pixmap(22, 22));
                update();
            });
}

void LauncherCard::mouseReleaseEvent(QMouseEvent* event)
{
    if (event->button() == Qt::LeftButton &&
        rect().contains(event->position().toPoint()))
    {
        setFocus(Qt::MouseFocusReason);
        Q_EMIT activated();
    }
    QFrame::mouseReleaseEvent(event);
}

void LauncherCard::keyPressEvent(QKeyEvent* event)
{
    if (event->key() == Qt::Key_Return || event->key() == Qt::Key_Enter ||
        event->key() == Qt::Key_Space)
    {
        Q_EMIT activated();
        event->accept();
        return;
    }
    QFrame::keyPressEvent(event);
}

void LauncherCard::paintEvent(QPaintEvent* event)
{
    QFrame::paintEvent(event);
    if (!hasFocus() || !FocusRingStyle::keyboard_focus())
        return;
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing);
    painter.setBrush(Qt::NoBrush);
    painter.setPen(QPen(Theme::instance().colour(colour_role::focus), 2.0));
    painter.drawRoundedRect(QRectF(rect()).adjusted(2, 2, -2, -2), 8, 8);
}

Launcher::Launcher(StudioBackend& backend, QWidget* parent)
    : QWidget(parent), backend_(backend)
{
    build();
    refresh();
    connect(&backend_, &StudioBackend::ports_changed, this,
            [this] { refresh(); });
    connect(&backend_, &StudioBackend::links_changed, this,
            [this] { refresh(); });
    connect(&Theme::instance(), &Theme::theme_changed, this,
            [this] { refresh(); });
}

void Launcher::build()
{
    auto* outer = new QHBoxLayout(this);
    outer->setContentsMargins(0, 0, 0, 0);
    outer->addStretch(1);
    auto* content = new QWidget(this);
    content->setFixedWidth(960);
    auto* layout = new QVBoxLayout(content);
    layout->setContentsMargins(40, 32, 40, 32);
    layout->setSpacing(16);

    readiness_ = new NoteBanner(NoteBanner::kind::ok, {}, content);
    readiness_->set_action(tr("View setup check"),
                           [this]
                           {
                               if (setup_offered_)
                                   Q_EMIT driver_setup_requested();
                               else
                                   Q_EMIT setup_check_requested();
                           });
    layout->addWidget(readiness_);

    auto* heading = new QLabel(tr("What do you want to do?"), content);
    heading->setFont(Theme::instance().font(text_style::display));
    layout->addWidget(heading);
    auto* sub =
        new QLabel(tr("Pick a job. Virtual Serial Studio builds it and tells "
                      "you what to open in your other program."),
                   content);
    sub->setFont(Theme::instance().font(text_style::bodyLarge));
    sub->setWordWrap(true);
    layout->addWidget(sub);

    auto* grid = new QGridLayout();
    grid->setHorizontalSpacing(16);
    grid->setVerticalSpacing(16);
    const auto add_card =
        [this, grid](int row, int column, const QString& icon,
                     const QString& title, const QString& description,
                     const QString& result, const QString& job)
    {
        auto* card = new LauncherCard(icon, title, description, result, this);
        connect(card, &LauncherCard::activated, this,
                [this, job] { Q_EMIT job_requested(job); });
        grid->addWidget(card, row, column);
    };
    add_card(0, 0, QStringLiteral("device"),
             tr("Give my program a serial port"),
             tr("Create a virtual port and get the name to open. "
                "Use it when a program needs a COM port and this PC "
                "has none."),
             tr("Result: a port name, such as COM7"), QStringLiteral("add"));
    add_card(0, 1, QStringLiteral("connect"), tr("Connect two programs"),
             tr("Make a linked pair so one program can talk to "
                "another, like a null-modem cable."),
             tr("Result: two ports, such as COM7 and COM8"),
             QStringLiteral("pair"));
    add_card(1, 0, QStringLiteral("share"),
             tr("Share one device with several programs"),
             tr("Open a real serial device once, then give each "
                "program its own port."),
             tr("Result: one port per program"), QStringLiteral("share"));
    add_card(1, 1, QStringLiteral("watch"), tr("Watch traffic"),
             tr("See what flows through a port without changing "
                "it."),
             tr("Result: a live monitor, read only"), QStringLiteral("watch"));
    layout->addLayout(grid);

    auto* saved = new QFrame(content);
    saved->setProperty("card", true);
    polish(saved);
    auto* saved_layout = new QHBoxLayout(saved);
    saved_layout->setContentsMargins(16, 12, 16, 12);
    saved_layout->setSpacing(12);
    auto* saved_title = new QLabel(tr("Saved setups"), saved);
    saved_title->setFont(semibold(Theme::instance().font(text_style::body)));
    saved_layout->addWidget(saved_title);
    recent_name_ = new QLabel(saved);
    recent_name_->setFont(Theme::instance().font(text_style::data));
    saved_layout->addWidget(recent_name_);
    recent_summary_ = new QLabel(saved);
    recent_summary_->setFont(Theme::instance().font(text_style::caption));
    recent_summary_->setProperty("muted", true);
    saved_layout->addWidget(recent_summary_);
    saved_layout->addStretch(1);
    no_recent_ = new QLabel(tr("No saved setups yet"), saved);
    no_recent_->setFont(Theme::instance().font(text_style::body));
    no_recent_->setProperty("muted", true);
    saved_layout->addWidget(no_recent_);
    recent_open_ = new QPushButton(tr("Open"), saved);
    recent_open_->setProperty("size", QStringLiteral("small"));
    polish(recent_open_);
    connect(recent_open_, &QPushButton::clicked, this,
            [this] { Q_EMIT open_recent_requested(recent_path_); });
    saved_layout->addWidget(recent_open_);
    recent_browse_ = new QPushButton(tr("Browse"), saved);
    recent_browse_->setProperty("size", QStringLiteral("small"));
    polish(recent_browse_);
    connect(recent_browse_, &QPushButton::clicked, this,
            [this] { Q_EMIT browse_requested(); });
    saved_layout->addWidget(recent_browse_);
    layout->addWidget(saved);

    auto* hint = new QLabel(
        tr("Need addresses, fault injection or port ownership? "
           "<a href=\"advanced\">Advanced mode</a> adds them to this "
           "same window. Nothing you have built changes."),
        content);
    hint->setTextFormat(Qt::RichText);
    hint->setFont(Theme::instance().font(text_style::body));
    hint->setWordWrap(true);
    connect(hint, &QLabel::linkActivated, this,
            [this](const QString&) { Q_EMIT advanced_requested(); });
    layout->addWidget(hint);
    layout->addStretch(1);

    outer->addWidget(content, 0);
    outer->addStretch(1);
}

void Launcher::refresh()
{
    const support::setup_report& report = setup_cache_.get(backend_);
    const bool windows = host_platform() == QLatin1String("windows");
    bool setup_offered = false;
    if (report.state == setup_report::readiness::not_checked)
    {
        readiness_->set_kind(NoteBanner::kind::warn);
        readiness_->set_text(tr(
            "Setup was not checked. Open the setup check to see what this PC "
            "needs."));
        if (readiness_->action_button() != nullptr)
            readiness_->action_button()->setText(tr("Open setup check"));
    }
    else if (!windows)
    {
        if (report.ports_ready)
        {
            readiness_->set_kind(NoteBanner::kind::ok);
            readiness_->set_text(
                tr("Ready. New ports are pseudo terminals that other programs "
                   "can open."));
            if (readiness_->action_button() != nullptr)
                readiness_->action_button()->setText(tr("View setup check"));
        }
        else
        {
            readiness_->set_kind(NoteBanner::kind::warn);
            readiness_->set_text(tr(
                "Setup needs attention. New ports may not be usable by other "
                "programs."));
            if (readiness_->action_button() != nullptr)
                readiness_->action_button()->setText(tr("Open setup check"));
        }
    }
    else if (report.ports_ready)
    {
        readiness_->set_kind(NoteBanner::kind::ok);
        readiness_->set_text(
            backend_.facts().driver_found
                ? tr("Ready. This PC can create ports that other programs "
                     "can open (Virtual Serial Studio driver detected).")
                : tr("Ready. This PC can create ports that other programs "
                     "can open (com0com detected)."));
        if (readiness_->action_button() != nullptr)
            readiness_->action_button()->setText(tr("View setup check"));
    }
    else if (driver_problem_offers_setup(report.problem))
    {
        // The first thing a new user sees: what is missing, and the one action
        // that fixes it.
        setup_offered = true;
        readiness_->set_kind(NoteBanner::kind::warn);
        readiness_->set_text(driver_problem_body(report.problem));
        if (readiness_->action_button() != nullptr)
            readiness_->action_button()->setText(
                tr("Set up Virtual Serial Studio"));
    }
    else
    {
        readiness_->set_kind(NoteBanner::kind::warn);
        readiness_->set_text(
            tr("Setup needs attention. Other programs cannot see new "
               "ports until a driver is installed."));
        if (readiness_->action_button() != nullptr)
            readiness_->action_button()->setText(tr("Open setup check"));
    }
    setup_offered_ = setup_offered;

    update_saved_setups();
    repaint();
}

void Launcher::update_saved_setups()
{
    const QSettings settings;
    const QStringList recent =
        settings.value(QStringLiteral("ui/recentFiles")).toStringList();
    recent_path_ = recent.value(0);
    const bool has_recent = !recent_path_.isEmpty();
    recent_name_->setVisible(has_recent);
    recent_summary_->setVisible(has_recent);
    recent_open_->setVisible(has_recent);
    no_recent_->setVisible(!has_recent);
    if (!has_recent)
    {
        recent_name_->clear();
        recent_summary_->clear();
        return;
    }
    recent_name_->setText(QFileInfo(recent_path_).fileName());
    const QString summary =
        settings.value(QStringLiteral("ui/recentSummary")).toString();
    recent_summary_->setText(summary.isEmpty() ? tr("Saved setup") : summary);
}

} // namespace vss::gui
