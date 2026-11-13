//
// Copyright (c) 2026 Virtual Serial Studio contributors
//
// SPDX-License-Identifier: GPL-3.0-or-later
//

#ifndef VSS_GUI_FLOWS_FLOW_DIALOG_HPP
#define VSS_GUI_FLOWS_FLOW_DIALOG_HPP

#include "backend/background_work.hpp"

#include <QDialog>
#include <QPair>
#include <QString>
#include <QVector>

#include <functional>

class QHBoxLayout;
class QLabel;
class QProgressBar;
class QPushButton;
class QScrollArea;
class QVBoxLayout;

namespace vss::gui
{

/// Shared chrome for the WP5 dialogs: title, one-sentence subtitle, a
/// scrollable body, and a sunken footer whose primary action is rightmost.
///
/// The body is a scroll area so 200 percent text never clips.  Escape is the
/// standard QDialog reject; callers set the primary button as the default.
class FlowDialog : public QDialog
{
    Q_OBJECT

public:
    FlowDialog(const QString& title, const QString& subtitle,
               QWidget* parent = nullptr);

    QVBoxLayout* body_layout() const noexcept { return body_layout_; }
    QWidget* body() const noexcept { return body_; }

    QPushButton* add_footer_button(const QString& text,
                                   const QString& role = {});
    QPushButton* primary_button() const noexcept { return primary_; }
    void set_primary_text(const QString& text);
    void set_primary_enabled(bool enabled);
    void set_subtitle_text(const QString& text);
    void set_title_text(const QString& text);
    void focus_first(QWidget* widget);
    void finish_layout(int preferred_height = 720);

    /// True while `run_in_background` has work on a worker thread.  The dialog
    /// cannot be closed meanwhile: the worker reports to it.
    bool busy() const noexcept { return work_.busy(); }

    /// Refuses to close while `run_in_background` work is running.
    void done(int result) override;

protected:
    /// What the busy row says while ports are made.
    static QString setting_up_text();

    /// Runs slow engine work (making a port installs a device in Windows and
    /// takes seconds) off the GUI thread.  `work` runs on a worker thread and
    /// touches no widget; `finished` follows on the GUI thread.  Meanwhile the
    /// body and every footer button are disabled, the primary button reads
    /// `primary_text`, and a progress bar and `status_text` say what is
    /// happening.  Everything is put back before `finished` runs.  False, and
    /// nothing runs, while earlier work is still running.
    bool run_in_background(std::function<void()> work,
                           std::function<void()> finished,
                           const QString& primary_text,
                           const QString& status_text);

private:
    void end_busy();
    void apply_theme();

    QLabel* title_ = nullptr;
    QLabel* subtitle_ = nullptr;
    QScrollArea* scroll_ = nullptr;
    QWidget* body_ = nullptr;
    QVBoxLayout* body_layout_ = nullptr;
    QHBoxLayout* footer_layout_ = nullptr;
    QPushButton* primary_ = nullptr;

    BackgroundWork work_;
    QWidget* busy_row_ = nullptr;
    QLabel* busy_label_ = nullptr;
    QString idle_primary_text_;
    QVector<QPair<QPushButton*, bool>> footer_state_;
};

} // namespace vss::gui

#endif // VSS_GUI_FLOWS_FLOW_DIALOG_HPP
