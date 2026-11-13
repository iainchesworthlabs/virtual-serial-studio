//
// Copyright (c) 2026 Virtual Serial Studio contributors
//
// SPDX-License-Identifier: GPL-3.0-or-later
//

#include "shell/status_bar.hpp"

#include "profiling/profiling_controller.hpp"
#include "shell/format.hpp"
#include "support/licence_dialog.hpp"
#include "support/setup_facts.hpp"
#include "theme/focus_ring_style.hpp"

#include <vss/core/profile.hpp>

#include <QHBoxLayout>
#include <QKeyEvent>
#include <QPaintEvent>
#include <QPainter>
#include <QSignalBlocker>

#include <algorithm>

namespace vss::gui
{
using namespace support;

EditionChip::EditionChip(QWidget* parent) : QAbstractButton(parent)
{
    setCursor(Qt::PointingHandCursor);
    setFocusPolicy(Qt::StrongFocus);
    // The chip is 22 px high; the target is 24.
    setMinimumHeight(24);
    auto* layout = new QHBoxLayout(this);
    layout->setContentsMargins(3, 1, 3, 1);
    layout->setSpacing(0);
    chip_ = new StatusChip(this);
    layout->addWidget(chip_, 0, Qt::AlignCenter);
    repaint_on_theme_change(this);
}

void EditionChip::set_look(const support::licence_chip& look)
{
    chip_->set_kind(support::chip_kind(look.tone));
    chip_->set_text(look.text);
    const QString name = support::edition_chip_name(look);
    setAccessibleName(name);
    setToolTip(name);
    updateGeometry();
    update();
}

QSize EditionChip::sizeHint() const
{
    const QSize chip = chip_->sizeHint();
    return QSize(chip.width() + 6, std::max(24, chip.height() + 2));
}

QSize EditionChip::minimumSizeHint() const
{
    return sizeHint();
}

void EditionChip::paintEvent(QPaintEvent* event)
{
    Q_UNUSED(event);
    const Theme& theme = Theme::instance();
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing);
    if (underMouse() || isDown())
    {
        painter.setPen(Qt::NoPen);
        painter.setBrush(
            theme.colour(isDown() ? colour_role::pressed : colour_role::hover));
        painter.drawRoundedRect(QRectF(rect()), height() / 2.0, height() / 2.0);
    }
    if (hasFocus() && FocusRingStyle::keyboard_focus())
    {
        painter.setBrush(Qt::NoBrush);
        painter.setPen(QPen(theme.colour(colour_role::focus), 2.0));
        painter.drawRoundedRect(QRectF(rect()).adjusted(1, 1, -1, -1),
                                height() / 2.0, height() / 2.0);
    }
}

void EditionChip::keyPressEvent(QKeyEvent* event)
{
    if (event->key() == Qt::Key_Return || event->key() == Qt::Key_Enter)
    {
        click();
        event->accept();
        return;
    }
    QAbstractButton::keyPressEvent(event);
}

StatusBar::StatusBar(StudioBackend& backend, QWidget* parent)
    : QStatusBar(parent), backend_(backend)
{
    setSizeGripEnabled(false);
    // A top-level window has rounded bottom corners on Windows 11 and macOS.
    // Content that sits against the edge of the bar is cut by the corner, so
    // inset both ends by the 12 px step of the spacing scale.
    const int inset = Theme::instance().space(2);
    setContentsMargins(inset, 2, inset, 2);
    setAccessibleName(tr("Status bar"));
    rate_timer_.start();
    last_ns_ = rate_timer_.nsecsElapsed();

    ports_ = new QLabel(this);
    links_ = new QLabel(this);
    rates_ = new QLabel(this);
    hub_ = new QLabel(this);
    for (QLabel* label : {ports_, links_, rates_, hub_})
    {
        label->setFont(Theme::instance().font(text_style::caption));
        label->setContentsMargins(0, 0, 12, 0);
        addWidget(label, 0);
    }

    dropped_ = new StatusChip(tr("0 dropped"), StatusChip::kind::waiting, this);
    dropped_->set_small(true);
    dropped_->hide();
    addWidget(dropped_, 0);

    // Shown only while the profiler is listening, so a person can always see
    // that it is and on which port.
    profiling_ = new StatusChip({}, StatusChip::kind::open, this);
    profiling_->set_small(true);
    profiling_->hide();
    addPermanentWidget(profiling_, 0);
    connect(&ProfilingController::instance(), &ProfilingController::changed,
            this, [this] { update_profiling(); });
    update_profiling();

    readiness_ =
        new StatusChip(tr("Setup ready"), StatusChip::kind::healthy, this);
    addPermanentWidget(readiness_, 0);

    // The edition: Community, Pro and so on. It opens the Licence page.
    edition_ = new EditionChip(this);
    edition_->setObjectName(QStringLiteral("editionChip"));
    connect(edition_, &QAbstractButton::clicked, this,
            &StatusBar::licence_requested);
    connect(&backend_, &StudioBackend::licence_changed, this,
            [this] { update_edition(); });
    addPermanentWidget(edition_, 0);
    update_edition();

    level_ = new SegmentedControl(this);
    level_->set_compact(true);
    level_->setAccessibleName(tr("Interface mode"));
    level_->add_item(tr("Simple"));
    level_->add_item(tr("Advanced"), 1);
    connect(level_, &SegmentedControl::current_changed, this,
            [this](int id) { Q_EMIT level_requested(id == 1); });
    addPermanentWidget(level_, 0);

    refresh();
}

void StatusBar::set_advanced(bool advanced)
{
    advanced_ = advanced;
    const QSignalBlocker blocker(level_);
    level_->set_current(advanced ? 1 : 0);
    dropped_->setVisible(advanced &&
                         dropped_->get_kind() == StatusChip::kind::waiting &&
                         !dropped_->text().isEmpty());
    refresh();
}

SegmentedControl* StatusBar::release_level_switch(QWidget* parent)
{
    removeWidget(level_);
    level_->setParent(parent);
    level_->set_compact(false);
    level_->show();
    return level_;
}

void StatusBar::refresh()
{
    VSS_PROFILE_ZONE("gui.status.refresh");
    const auto ports = backend_.ports();
    const auto links = backend_.links();

    std::uint64_t rx = 0;
    std::uint64_t tx = 0;
    std::uint64_t dropped = 0;
    std::uint64_t owner_dropped = 0;
    for (const auto& port : ports)
    {
        rx += port.stats.bytes_rx;
        tx += port.stats.bytes_tx;
        dropped += port.stats.dropped;
    }
    for (const auto& link : links)
    {
        dropped += link.stats.dropped;
        if (link.options.policy == vss::merge_policy::owner)
            owner_dropped += link.stats.dropped;
    }

    const qint64 now = rate_timer_.nsecsElapsed();
    const double seconds =
        static_cast<double>(now - last_ns_) / 1'000'000'000.0;
    last_ns_ = now;
    double rx_rate = 0.0;
    double tx_rate = 0.0;
    if (seconds > 0.0)
    {
        // The totals are sums over the ports that exist now, so they fall
        // when a port is removed; a fall is no traffic, not a wrapped count.
        const std::uint64_t rx_delta =
            rx >= previous_rx_ ? rx - previous_rx_ : 0;
        const std::uint64_t tx_delta =
            tx >= previous_tx_ ? tx - previous_tx_ : 0;
        rx_rate = static_cast<double>(rx_delta) / seconds;
        tx_rate = static_cast<double>(tx_delta) / seconds;
    }
    previous_rx_ = rx;
    previous_tx_ = tx;

    const bool empty = ports.empty();
    if (empty)
        ports_->setText(tr("No ports yet"));
    else
        ports_->setText(
            tr("Ports %1").arg(static_cast<qulonglong>(ports.size())));
    links_->setText(tr("Links %1").arg(static_cast<qulonglong>(links.size())));
    rates_->setText(
        tr("RX %1   TX %2").arg(format_rate(rx_rate), format_rate(tx_rate)));
    hub_->setText(backend_.hub_running() ? tr("Hub running")
                                         : tr("Hub stopped"));
    links_->setVisible(!empty);
    rates_->setVisible(!empty);
    hub_->setVisible(!empty);

    if (advanced_ && dropped > 0)
    {
        if (owner_dropped > 0 && owner_dropped == dropped)
        {
            dropped_->set_text(
                tr("%1 dropped, not owner").arg(format_bytes(dropped)));
            dropped_->setToolTip(
                tr("Bytes from a port that is not the owner were "
                   "not forwarded."));
        }
        else
        {
            dropped_->set_text(tr("%1 dropped").arg(format_bytes(dropped)));
            dropped_->setToolTip({});
        }
        dropped_->show();
    }
    else
    {
        dropped_->hide();
    }

    update_readiness();
}

void StatusBar::update_profiling()
{
    const auto& controller = ProfilingController::instance();
    const QString text = controller.chip_text();
    if (text.isEmpty())
    {
        profiling_->hide();
        return;
    }
    const auto status = controller.status();
    profiling_->set_kind(status.current == vss::profiling::state::paused
                             ? StatusChip::kind::closed
                             : (status.profiler_connected
                                    ? StatusChip::kind::connected
                                    : StatusChip::kind::open));
    profiling_->set_text(text);
    profiling_->setAccessibleName(controller.summary());
    profiling_->setToolTip(controller.summary());
    profiling_->show();
}

void StatusBar::update_edition()
{
    edition_->set_look(support::edition_chip(backend_.licence().status));
}

void StatusBar::update_readiness()
{
    const support::setup_report& report = setup_cache_.get(backend_);
    switch (report.state)
    {
    case setup_report::readiness::ready:
        readiness_->set_kind(StatusChip::kind::healthy);
        readiness_->set_text(tr("Setup ready"));
        break;
    case setup_report::readiness::needs_attention:
        readiness_->set_kind(StatusChip::kind::waiting);
        readiness_->set_text(tr("Setup needs attention"));
        break;
    case setup_report::readiness::not_checked:
        readiness_->set_kind(StatusChip::kind::closed);
        readiness_->set_text(tr("Setup not checked"));
        break;
    }
    readiness_->setAccessibleName(readiness_->text());
    readiness_->setToolTip(tr("Open Tools, Setup check for the details."));
}

} // namespace vss::gui
