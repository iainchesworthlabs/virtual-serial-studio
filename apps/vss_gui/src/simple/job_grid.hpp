//
// Copyright (c) 2026 Virtual Serial Studio contributors
//
// SPDX-License-Identifier: GPL-3.0-or-later
//

#pragma once

#include "backend/studio_backend.hpp"
#include "simple/setup_kinds.hpp"
#include "support/setup_facts.hpp"

#include <QFrame>
#include <QHash>
#include <QString>
#include <QWidget>

class QKeyEvent;
class QLabel;
class QMouseEvent;
class QPaintEvent;

namespace vss::gui
{
class NoteBanner;
}

namespace vss::gui::simple
{

/// One job on the grid: an icon tile, a title and a short description.  It
/// behaves as a button: click, Enter or Space activates it.
class JobTile final : public QFrame
{
    Q_OBJECT

public:
    JobTile(const QString& icon, const QString& title,
            const QString& description, QWidget* parent = nullptr);

    QString title() const { return title_; }

Q_SIGNALS:
    void activated();

protected:
    void mousePressEvent(QMouseEvent* event) override;
    void mouseReleaseEvent(QMouseEvent* event) override;
    void keyPressEvent(QKeyEvent* event) override;
    void paintEvent(QPaintEvent* event) override;
    void changeEvent(QEvent* event) override;

private:
    void apply_tile_style();

    QString title_;
    QString icon_;
    QFrame* tile_ = nullptr;
    QLabel* glyph_ = nullptr;
    bool pressed_ = false;
};

/// "What do you want to do?": the readiness note and every job, in groups.
class JobGrid final : public QWidget
{
    Q_OBJECT

public:
    explicit JobGrid(StudioBackend& backend, QWidget* parent = nullptr);

    void refresh();
    /// Forgets the last readiness answer, so the next `refresh` asks again.
    void invalidate_setup() { setup_cache_.invalidate(); }

    /// The tile of `kind`, for tests and focus.
    JobTile* tile(job kind) const;

Q_SIGNALS:
    void job_chosen(vss::gui::simple::job kind);
    void watch_requested();
    void check_requested();
    /// The readiness banner offered "Set up Virtual Serial Studio" and it was
    /// chosen.
    void driver_setup_requested();
    void open_requested();
    void examples_requested();

private:
    StudioBackend& backend_;
    support::setup_report_cache setup_cache_;
    bool setup_offered_ = false;
    NoteBanner* readiness_ = nullptr;
    QHash<int, JobTile*> tiles_;
};

} // namespace vss::gui::simple
