//
// Copyright (c) 2026 Virtual Serial Studio contributors
//
// SPDX-License-Identifier: GPL-3.0-or-later
//

#ifndef VSS_GUI_SHELL_STATUS_BAR_HPP
#define VSS_GUI_SHELL_STATUS_BAR_HPP

#include "backend/plugin_backend.hpp"
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

namespace vss::gui::updates
{
class UpdateService;
} // namespace vss::gui::updates

namespace vss::gui
{

/// A status chip that is also a button. The chip is the only thing drawn; the
/// button adds the keyboard focus ring and a 24 px target.
class ChipButton : public QAbstractButton
{
    Q_OBJECT

public:
    explicit ChipButton(QWidget* parent = nullptr);

    /// Shows `text` as a chip of `kind`, and names the button `name`.
    void set_content(StatusChip::kind kind, const QString& text,
                     const QString& name);
    StatusChip* chip() const noexcept { return chip_; }

    QSize sizeHint() const override;
    QSize minimumSizeHint() const override;

protected:
    void paintEvent(QPaintEvent* event) override;
    void keyPressEvent(QKeyEvent* event) override;

private:
    StatusChip* chip_ = nullptr;
};

/// The edition chip, which opens the Licence page.
class EditionChip final : public ChipButton
{
    Q_OBJECT

public:
    explicit EditionChip(QWidget* parent = nullptr);

    /// Shows `look`, and names the button "Edition: <text>. Open the licence
    /// page."
    void set_look(const support::licence_chip& look);
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

    /// Where the plugin host's snapshot comes from, for the chip that says a
    /// plugin reads data. Null: no chip.
    void set_plugin_backend(PluginBackend* host);
    /// "Recording" or "Reading data", while any plugin reads data. Hidden
    /// otherwise; it cannot be dismissed.
    ChipButton* plugin_chip() const noexcept { return plugin_chip_; }
    /// Gives the bar the update service, once, or null for none. With a
    /// service the bar shows the "Update available" chip to the left of the
    /// edition chip while the service says it is due.
    void set_update_service(updates::UpdateService* service);
    /// The update chip. It is hidden while no service is given or no update
    /// is due.
    ChipButton* update_chip() const noexcept { return update_; }

Q_SIGNALS:
    void level_requested(bool advanced);
    /// The edition chip was pressed.
    void licence_requested();
    /// The chip that says a plugin reads data was pressed.
    void plugins_requested();
    /// The update chip was pressed.
    void update_requested();

private:
    void update_readiness();
    void update_profiling();
    void update_debug_access();
    void update_edition();
    void update_plugin_chip();
    void update_update_chip();

    /// Holds the counters and the dropped chip.  `QStatusBar` hides and shows
    /// its plain widgets as one each time a message comes and goes, and shows
    /// a hidden one again with the message still up.  So the labels inside are
    /// hidden and shown here, never in the bar.
    QWidget* counters_ = nullptr;

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
    ChipButton* plugin_chip_ = nullptr;
    ChipButton* debug_chip_ = nullptr;
    PluginBackend* plugins_ = nullptr;
    updates::UpdateService* updates_ = nullptr;
    ChipButton* update_ = nullptr;
    SegmentedControl* level_ = nullptr;

    QElapsedTimer rate_timer_;
    qint64 last_ns_ = 0;
    std::uint64_t previous_rx_ = 0;
    std::uint64_t previous_tx_ = 0;
    bool advanced_ = false;
};

} // namespace vss::gui

#endif // VSS_GUI_SHELL_STATUS_BAR_HPP
