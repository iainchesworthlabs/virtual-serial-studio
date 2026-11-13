//
// Copyright (c) 2026 Virtual Serial Studio contributors
//
// SPDX-License-Identifier: GPL-3.0-or-later
//

#ifndef VSS_GUI_ADVANCED_JSON_EDITOR_HPP
#define VSS_GUI_ADVANCED_JSON_EDITOR_HPP

#include "advanced/json_validation.hpp"
#include "backend/background_work.hpp"
#include "backend/studio_backend.hpp"

#include <QPlainTextEdit>
#include <QString>
#include <QVector>
#include <QWidget>

class QLabel;
class QPaintEvent;
class QPushButton;
class QResizeEvent;

namespace vss::gui
{

class CommandPanel;
class StatusChip;

/// The fixed-font editor with the line-number gutter and the error highlight.
class JsonCodeEdit final : public QPlainTextEdit
{
public:
    explicit JsonCodeEdit(QWidget* parent = nullptr);

    void set_error_line(int line);
    int error_line() const noexcept { return error_line_; }
    void go_to_line(int line);
    int gutter_width() const;
    void paint_gutter(QPaintEvent* event);

protected:
    void resizeEvent(QResizeEvent* event) override;

private:
    void update_gutter(const QRect& rect, int dy);
    void highlight_error_line();

    QWidget* gutter_ = nullptr;
    int error_line_ = 0;
};

/// The Advanced bottom tab: live topology JSON with validation and checks.
///
/// It is a widget, not a dialog, so the shell can host it in a dock today and
/// WP4 can move it into the Monitor tab set without rework.  It never applies
/// a document that `validate_json_document` did not accept.
class TopologyJsonEditor final : public QWidget
{
    Q_OBJECT

public:
    explicit TopologyJsonEditor(StudioBackend& backend,
                                QWidget* parent = nullptr);

    /// Loads the current `studio::snapshot()` as the clean baseline.
    void load_from_backend();
    void reload_if_clean();
    void set_document_path(const QString& path);

    QString text() const;
    void set_text(const QString& text);
    void revalidate();

    bool is_dirty() const;
    bool can_apply() const;
    /// Starts applying the text to the engine on a worker thread and returns
    /// true; `applied` or an error message follows on the GUI thread.  False
    /// when nothing can be applied or an apply is already running.
    bool apply();
    bool busy() const noexcept { return work_.busy(); }
    void revert();
    void format();

    const json_validation& validation() const noexcept { return validation_; }
    QVector<json_check> checks() const;
    QString error_summary() const;

    JsonCodeEdit* editor() const noexcept { return editor_; }
    QPushButton* apply_button() const noexcept { return apply_; }
    QPushButton* revert_button() const noexcept { return revert_; }
    QPushButton* format_button() const noexcept { return format_; }
    QPushButton* go_to_line_button() const noexcept { return go_to_line_; }
    QPushButton* fix_button() const noexcept { return fix_; }
    StatusChip* status_chip() const noexcept { return status_; }
    CommandPanel* command_panel() const noexcept { return command_; }

    void copy_as_command();
    bool export_file();

Q_SIGNALS:
    void message(const QString& text);
    void applied();

private:
    void build();
    QWidget* build_checks();
    QWidget* build_error_panel();
    void update_ui();
    void update_checks();
    void update_error_panel();
    void schedule_validation();
    void update_command();
    void show_apply_error(const QString& text);
    QString command_file_name() const;

    StudioBackend& backend_;
    QString path_;
    QString baseline_;
    json_validation validation_;
    QString apply_error_summary_;
    QString apply_error_detail_;
    /// Applying makes ports, which takes seconds in Windows, so it runs on a
    /// worker thread.
    BackgroundWork work_;
    bool applying_ = false;

    JsonCodeEdit* editor_ = nullptr;
    StatusChip* status_ = nullptr;
    QLabel* caption_ = nullptr;
    QPushButton* revert_ = nullptr;
    QPushButton* format_ = nullptr;
    QPushButton* apply_ = nullptr;
    QWidget* checks_holder_ = nullptr;
    QWidget* error_panel_ = nullptr;
    QLabel* error_summary_ = nullptr;
    QLabel* error_detail_ = nullptr;
    QPushButton* go_to_line_ = nullptr;
    QPushButton* fix_ = nullptr;
    CommandPanel* command_ = nullptr;
};

} // namespace vss::gui

#endif // VSS_GUI_ADVANCED_JSON_EDITOR_HPP
