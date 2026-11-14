//
// Copyright (c) 2026 Virtual Serial Studio contributors
//
// SPDX-License-Identifier: GPL-3.0-or-later
//

#ifndef VSS_GUI_SHELL_STATUS_BAR_HPP
#define VSS_GUI_SHELL_STATUS_BAR_HPP

#include "backend/studio_backend.hpp"
#include "support/licence_text.hpp"
#include "support/setup_facts.hpp"
#include "widgets/chips.hpp"
#include "widgets/controls.hpp"

#include <QAbstractButton>
#include <QElapsedTimer>
#include <QLabel>
#include <QStatusBar>

#include <cstdint>

namespace vss::gui
{

/// The edition chip: a status chip that is also a button, which opens the
/// Licence page. The chip is the only thing drawn; the button adds the
/// keyboard focus ring and a 24 px target.
class EditionChip final : public QAbstractButton
{
    Q_OBJECT

public:
    explicit EditionChip(QWidget* parent = nullptr);

    /// Shows `look`, and names the button "Edition: <text>. Open the licence
    /// page."
    void set_look(const support::licence_chip& look);
    StatusChip* chip() const noexcept { return chip_; }

    QSize sizeHint() const override;
    QSize minimumSizeHint() const override;

protected:
    void paintEvent(QPaintEvent* event) override;
    void keyPressEvent(QKeyEvent* event) override;

private:
    StatusChip* chip_ = nullptr;
};

/// Ports, links, rates, hub state, readiness and the level switch.
class StatusBar final : public QStatusBar
{
    Q_OBJECT

public:
    explicit StatusBar(StudioBackend& backend, QWidget* parent = nullptr);

    void set_advanced(bool advanced);

    /// Moves the Simple and Advanced switch out of the bar and into
    /// `parent`, so the window can show it in its header row.  The bar keeps
    /// driving it.
    SegmentedControl* release_level_switch(QWidget* parent);
    void refresh();
    /// Forgets the last readiness answer, so the next `refresh` asks again.
    void invalidate_setup() { setup_cache_.invalidate(); }

    EditionChip* edition_chip() const noexcept { return edition_; }

Q_SIGNALS:
    void level_requested(bool advanced);
    /// The edition chip was pressed.
    void licence_requested();

private:
    void update_readiness();
    void update_profiling();
    void update_edition();

    StudioBackend& backend_;
    support::setup_report_cache setup_cache_;
    QLabel* ports_ = nullptr;
    QLabel* links_ = nullptr;
    QLabel* rates_ = nullptr;
    QLabel* hub_ = nullptr;
    StatusChip* readiness_ = nullptr;
    StatusChip* dropped_ = nullptr;
    StatusChip* profiling_ = nullptr;
    EditionChip* edition_ = nullptr;
    SegmentedControl* level_ = nullptr;

    QElapsedTimer rate_timer_;
    qint64 last_ns_ = 0;
    std::uint64_t previous_rx_ = 0;
    std::uint64_t previous_tx_ = 0;
    bool advanced_ = false;
};

} // namespace vss::gui

#endif // VSS_GUI_SHELL_STATUS_BAR_HPP
