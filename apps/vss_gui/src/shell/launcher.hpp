//
// Copyright (c) 2026 Virtual Serial Studio contributors
//
// SPDX-License-Identifier: GPL-3.0-or-later
//

#ifndef VSS_GUI_SHELL_LAUNCHER_HPP
#define VSS_GUI_SHELL_LAUNCHER_HPP

#include "backend/studio_backend.hpp"
#include "support/setup_facts.hpp"
#include "widgets/notes.hpp"

#include <QFrame>
#include <QString>
#include <QWidget>

class QLabel;
class QPushButton;

namespace vss::gui
{

/// One of the four jobs on the launcher.
class LauncherCard final : public QFrame
{
    Q_OBJECT

public:
    LauncherCard(const QString& icon, const QString& title,
                 const QString& description, const QString& result,
                 QWidget* parent = nullptr);

    QString title() const { return title_; }

Q_SIGNALS:
    void activated();

protected:
    void mouseReleaseEvent(QMouseEvent* event) override;
    void keyPressEvent(QKeyEvent* event) override;
    void paintEvent(QPaintEvent* event) override;

private:
    QString title_;
};

/// Empty-state page: readiness, four jobs, saved setups and the Advanced hint.
class Launcher final : public QWidget
{
    Q_OBJECT

public:
    explicit Launcher(StudioBackend& backend, QWidget* parent = nullptr);

    void refresh();
    /// Forgets the last readiness answer, so the next `refresh` asks again.
    void invalidate_setup() { setup_cache_.invalidate(); }

Q_SIGNALS:
    void job_requested(const QString& job);
    void setup_check_requested();
    /// The banner offered "Set up Virtual Serial Studio" and it was chosen.
    void driver_setup_requested();
    void open_recent_requested(const QString& path);
    void browse_requested();
    void advanced_requested();

private:
    void build();
    void update_saved_setups();

    StudioBackend& backend_;
    support::setup_report_cache setup_cache_;
    bool setup_offered_ = false;
    NoteBanner* readiness_ = nullptr;
    QLabel* recent_name_ = nullptr;
    QLabel* recent_summary_ = nullptr;
    QPushButton* recent_open_ = nullptr;
    QPushButton* recent_browse_ = nullptr;
    QLabel* no_recent_ = nullptr;
    QString recent_path_;
};

} // namespace vss::gui

#endif // VSS_GUI_SHELL_LAUNCHER_HPP
