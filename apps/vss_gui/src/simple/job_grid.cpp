//
// Copyright (c) 2026 Virtual Serial Studio contributors
//
// SPDX-License-Identifier: GPL-3.0-or-later
//

#include "simple/job_grid.hpp"

#include "backend/driver_error.hpp"
#include "support/setup_facts.hpp"
#include "theme/focus_ring_style.hpp"
#include "theme/icon_provider.hpp"
#include "widgets/common.hpp"
#include "widgets/notes.hpp"

#include <QGridLayout>
#include <QHBoxLayout>
#include <QKeyEvent>
#include <QLabel>
#include <QMouseEvent>
#include <QPainter>
#include <QPushButton>
#include <QScrollArea>
#include <QVBoxLayout>

namespace vss::gui::simple
{

namespace
{

QFont semibold(QFont font)
{
    font.setWeight(QFont::DemiBold);
    return font;
}

QString icon_of(job kind)
{
    switch (kind)
    {
    case job::port:
        return QStringLiteral("add");
    case job::pair:
        return QStringLiteral("connect");
    case job::attach:
        return QStringLiteral("device");
    case job::share:
        return QStringLiteral("share");
    case job::talk:
        return QStringLiteral("send");
    case job::network:
        return QStringLiteral("export");
    case job::bus:
        return QStringLiteral("connect");
    case job::combine:
        return QStringLiteral("run");
    case job::turns:
        return QStringLiteral("settings");
    case job::custom:
        break;
    }
    return QStringLiteral("info");
}

} // namespace

JobTile::JobTile(const QString& icon, const QString& title,
                 const QString& description, QWidget* parent)
    : QFrame(parent), title_(title), icon_(icon)
{
    setObjectName(QStringLiteral("jobTile"));
    setProperty("card", true);
    polish(this);
    setFocusPolicy(Qt::StrongFocus);
    setCursor(Qt::PointingHandCursor);
    setAccessibleName(title);
    setAccessibleDescription(description);
    setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);

    auto* layout = new QHBoxLayout(this);
    layout->setContentsMargins(16, 14, 16, 14);
    layout->setSpacing(12);

    tile_ = new QFrame(this);
    tile_->setFixedSize(32, 32);
    auto* tile_layout = new QVBoxLayout(tile_);
    tile_layout->setContentsMargins(0, 0, 0, 0);
    glyph_ = new QLabel(tile_);
    glyph_->setAlignment(Qt::AlignCenter);
    tile_layout->addWidget(glyph_, 0, Qt::AlignCenter);
    layout->addWidget(tile_, 0, Qt::AlignTop);

    auto* column = new QVBoxLayout();
    column->setContentsMargins(0, 0, 0, 0);
    column->setSpacing(4);
    auto* heading = new QLabel(title, this);
    heading->setFont(semibold(Theme::instance().font(text_style::bodyLarge)));
    heading->setWordWrap(true);
    auto* body = new QLabel(description, this);
    body->setFont(Theme::instance().font(text_style::caption));
    body->setProperty("muted", true);
    body->setWordWrap(true);
    column->addWidget(heading);
    column->addWidget(body);
    column->addStretch(1);
    layout->addLayout(column, 1);

    for (auto* label : {heading, body})
        label->setAttribute(Qt::WA_TransparentForMouseEvents);
    // Room for the title and three lines of description at any text size,
    // so a row of tiles never squashes its text.
    setMinimumHeight(QFontMetrics(heading->font()).height() +
                     3 * QFontMetrics(body->font()).lineSpacing() + 4 + 28);
    glyph_->setAttribute(Qt::WA_TransparentForMouseEvents);
    tile_->setAttribute(Qt::WA_TransparentForMouseEvents);

    apply_tile_style();
    connect(&Theme::instance(), &Theme::theme_changed, this,
            [this] { apply_tile_style(); });
}

void JobTile::apply_tile_style()
{
    const auto colour = [](colour_role role)
    { return Theme::instance().colour(role).name(QColor::HexRgb); };
    tile_->setStyleSheet(
        QStringLiteral("QFrame { background: %1; border: none; "
                       "border-radius: 9px; }")
            .arg(colour(isEnabled() ? colour_role::accentSoft
                                    : colour_role::sunken)));
    glyph_->setPixmap(
        IconProvider::instance()
            .icon(icon_,
                  isEnabled() ? colour_role::accentStrong : colour_role::ink3,
                  18)
            .pixmap(18, 18));
    update();
}

void JobTile::changeEvent(QEvent* event)
{
    QFrame::changeEvent(event);
    if (event->type() == QEvent::EnabledChange)
        apply_tile_style();
}

void JobTile::mousePressEvent(QMouseEvent* event)
{
    // Accepting the press is what lets the release reach this tile.
    if (event->button() == Qt::LeftButton)
    {
        pressed_ = true;
        event->accept();
        return;
    }
    QFrame::mousePressEvent(event);
}

void JobTile::mouseReleaseEvent(QMouseEvent* event)
{
    const bool was_pressed = pressed_;
    pressed_ = false;
    if (was_pressed && event->button() == Qt::LeftButton &&
        rect().contains(event->position().toPoint()))
    {
        setFocus(Qt::MouseFocusReason);
        Q_EMIT activated();
        event->accept();
        return;
    }
    QFrame::mouseReleaseEvent(event);
}

void JobTile::keyPressEvent(QKeyEvent* event)
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

void JobTile::paintEvent(QPaintEvent* event)
{
    QFrame::paintEvent(event);
    if (!hasFocus() || !FocusRingStyle::keyboard_focus())
        return;
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing);
    painter.setBrush(Qt::NoBrush);
    painter.setPen(QPen(Theme::instance().colour(colour_role::focus), 2.0));
    painter.drawRoundedRect(QRectF(rect()).adjusted(2, 2, -2, -2), 10, 10);
}

JobGrid::JobGrid(StudioBackend& backend, QWidget* parent)
    : QWidget(parent), backend_(backend)
{
    setObjectName(QStringLiteral("simpleJobs"));
    auto* outer = new QVBoxLayout(this);
    outer->setContentsMargins(0, 0, 0, 0);
    auto* scroll = new QScrollArea(this);
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);
    outer->addWidget(scroll);

    auto* page = new QWidget(scroll);
    auto* centre = new QHBoxLayout(page);
    centre->setContentsMargins(0, 0, 0, 0);
    auto* content = new QWidget(page);
    content->setMaximumWidth(920);
    centre->addWidget(content, 1);
    scroll->setWidget(page);

    auto* layout = new QVBoxLayout(content);
    layout->setContentsMargins(24, 24, 24, 32);
    layout->setSpacing(12);

    auto* heading =
        make_label(tr("What do you want to do?"), text_style::display, content);
    heading->setAccessibleName(heading->text());
    layout->addWidget(heading);
    layout->addWidget(make_label(
        tr("Choose a job. Studio sets it up and tells you which port to open "
           "in your program. Everything you set up is listed on the left, "
           "where you can change or remove it."),
        text_style::bodyLarge, content));

    readiness_ = new NoteBanner(NoteBanner::kind::ok, {}, content);
    readiness_->set_action(tr("Details"),
                           [this]
                           {
                               if (setup_offered_)
                                   Q_EMIT driver_setup_requested();
                               else
                                   Q_EMIT check_requested();
                           });
    layout->addWidget(readiness_);

    const bool bus_available = backend_.caps().shared_bus;
    const bool bridge_available = backend_.caps().bridge;
    const auto add_group =
        [this, content, layout](const QString& name,
                                const QList<QPair<int, JobTile*>>& tiles)
    {
        auto* label = new QLabel(name.toUpper(), content);
        label->setProperty("group", true);
        label->setFont(semibold(Theme::instance().font(text_style::caption)));
        label->setProperty("muted", true);
        polish(label);
        layout->addSpacing(8);
        layout->addWidget(label);
        auto* grid = new QGridLayout();
        grid->setHorizontalSpacing(12);
        grid->setVerticalSpacing(12);
        int index = 0;
        for (const auto& [id, tile] : tiles)
        {
            grid->addWidget(tile, index / 2, index % 2);
            if (id >= 0)
                tiles_.insert(id, tile);
            ++index;
        }
        grid->setColumnStretch(0, 1);
        grid->setColumnStretch(1, 1);
        layout->addLayout(grid);
    };
    const auto job_tile = [this, content](job kind)
    {
        auto* tile = new JobTile(icon_of(kind), job_title(kind),
                                 job_description(kind), content);
        connect(tile, &JobTile::activated, this,
                [this, kind] { Q_EMIT job_chosen(kind); });
        return QPair<int, JobTile*>(static_cast<int>(kind), tile);
    };

    add_group(tr("My program needs a serial port"),
              {job_tile(job::port), job_tile(job::pair)});
    add_group(tr("I have a serial device"),
              {job_tile(job::attach), job_tile(job::share), job_tile(job::talk),
               job_tile(job::network)});
    add_group(
        tr("Several programs or devices at once"),
        {job_tile(job::bus), job_tile(job::combine), job_tile(job::turns)});

    auto* watch = new JobTile(
        QStringLiteral("watch"), tr("See the data on a port"),
        tr("See what goes in and out of a port you already set up. Read only, "
           "so nothing changes for the programs using it."),
        content);
    connect(watch, &JobTile::activated, this,
            [this] { Q_EMIT watch_requested(); });
    auto* check = new JobTile(
        QStringLiteral("done"), tr("Check this PC is ready"),
        tr("Find out whether this PC can make ports that other programs can "
           "open, and what to do if not."),
        content);
    connect(check, &JobTile::activated, this,
            [this] { Q_EMIT check_requested(); });
    add_group(tr("Look and check"), {{-1, watch}, {-1, check}});

    if (!bus_available)
    {
        tiles_.value(static_cast<int>(job::bus))->setEnabled(false);
        tiles_.value(static_cast<int>(job::bus))
            ->setToolTip(tr("Not available in this build"));
    }
    if (!bridge_available)
    {
        tiles_.value(static_cast<int>(job::network))->setEnabled(false);
        tiles_.value(static_cast<int>(job::network))
            ->setToolTip(tr("Not available in this build"));
    }

    auto* more = new QHBoxLayout();
    more->setSpacing(8);
    auto* open = new QPushButton(tr("Open a saved setup…"), content);
    set_role(open, "secondary");
    connect(open, &QPushButton::clicked, this,
            [this] { Q_EMIT open_requested(); });
    auto* examples = new QPushButton(tr("Start from an example…"), content);
    set_role(examples, "secondary");
    connect(examples, &QPushButton::clicked, this,
            [this] { Q_EMIT examples_requested(); });
    more->addWidget(open);
    more->addWidget(examples);
    more->addStretch(1);
    layout->addSpacing(8);
    layout->addLayout(more);
    layout->addStretch(1);

    refresh();
}

JobTile* JobGrid::tile(job kind) const
{
    return tiles_.value(static_cast<int>(kind), nullptr);
}

void JobGrid::refresh()
{
    const support::setup_report& report = setup_cache_.get(backend_);
    setup_offered_ = false;
    if (report.state != support::setup_report::readiness::not_checked &&
        report.ports_ready)
    {
        readiness_->set_kind(NoteBanner::kind::ok);
        readiness_->set_text(
            tr("This PC can make ports that other programs can open."));
        if (readiness_->action_button() != nullptr)
            readiness_->action_button()->setText(tr("Details"));
    }
    else if (driver_problem_offers_setup(report.problem))
    {
        // A new user's first screen: what is missing and the one action that
        // fixes it.
        setup_offered_ = true;
        readiness_->set_kind(NoteBanner::kind::warn);
        readiness_->set_text(driver_problem_body(report.problem));
        if (readiness_->action_button() != nullptr)
            readiness_->action_button()->setText(
                tr("Set up Virtual Serial Studio"));
    }
    else
    {
        readiness_->set_kind(NoteBanner::kind::warn);
        readiness_->set_text(tr("Other programs cannot see new ports until a "
                                "driver is installed."));
        if (readiness_->action_button() != nullptr)
            readiness_->action_button()->setText(tr("Open setup check"));
    }
}

} // namespace vss::gui::simple
