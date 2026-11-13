//
// Copyright (c) 2026 Virtual Serial Studio contributors
//
// SPDX-License-Identifier: GPL-3.0-or-later
//
// "Create a support bundle" (docs/gui-design/docs/screens/
// 09-licence-and-support.md, "Support bundle"): the checklist of what to
// include, the preview of every file, and a local stored ZIP. Nothing is sent
// anywhere. The preview and the ZIP hold the same bytes because both come
// from one call, `build_bundle(...).files()`.
//

#pragma once

#include "backend/studio_backend.hpp"
#include "support/bundle.hpp"
#include "support/event_log.hpp"

#include <QDialog>
#include <QString>
#include <QVector>

#include <functional>

class QCheckBox;
class QFrame;
class QLabel;
class QListWidget;
class QPlainTextEdit;
class QPushButton;

namespace vss::gui
{
class NoteBanner;
} // namespace vss::gui

namespace vss::gui::support
{

/// The most the preview shows of one file. A longer file is shown cut at this
/// size, and the saved file is still complete.
inline constexpr qsizetype bundle_preview_limit = 256 * 1024;

class SupportBundleDialog final : public QDialog
{
    Q_OBJECT

public:
    /// Chooses where to save the bundle and returns the path; empty when the
    /// person chose none. The default opens the native save dialog.
    using path_picker = std::function<QString(const QString& suggested)>;
    /// Shows a folder in the system's file manager.
    using folder_opener = std::function<void(const QString& folder)>;

    SupportBundleDialog(StudioBackend& backend, const EventLog& log,
                        QWidget* parent = nullptr);

    /// Replaces the native save dialog, so a test can supply a path.
    void set_path_picker(path_picker picker) { picker_ = std::move(picker); }
    /// Replaces the file manager, so a test does not open one.
    void set_folder_opener(folder_opener opener)
    {
        opener_ = std::move(opener);
    }

    bundle_selection selection() const;
    /// The number of files the bundle holds, the manifest included.
    int selected_file_count() const;
    QString bundle_name() const;
    QString suggested_path() const;

    /// The files shown in the preview and written by `write_bundle`, in the
    /// order they are written.
    const QVector<bundle_file>& files() const noexcept { return files_; }

    /// Writes `files()` as one ZIP. No file dialog is involved, so a test can
    /// validate the archive itself.
    bool write_bundle(const QString& path, QString* error = nullptr);

    /// "Save bundle": asks where, writes, and says so. A path that cannot be
    /// written ends in the failure dialog.
    void save();
    /// "Copy environment only".
    void copy_environment();

    bool available() const noexcept { return available_; }

    /// The check boxes of the checklist, in the order of the rows.
    QVector<QCheckBox*> checks() const;
    /// The check box of one row by its object name, such as
    /// "bundleLicenseeName", or null when the row is absent.
    QCheckBox* check(const QString& object_name) const;
    QListWidget* file_list() const noexcept { return list_; }
    QPlainTextEdit* preview() const noexcept { return preview_; }
    /// "Preview shortened. The saved file is complete."
    QLabel* shortened_note() const noexcept { return shortened_; }
    QLabel* file_count_label() const noexcept { return count_; }
    QPushButton* copy_button() const noexcept { return copy_; }
    QPushButton* cancel_button() const noexcept { return cancel_; }
    QPushButton* save_button() const noexcept { return save_; }

private:
    struct row
    {
        QCheckBox* check = nullptr;
        bool bundle_selection::* member = nullptr;
    };

    void build();
    QFrame* add_row(QWidget* parent, const QString& object_name,
                    bool bundle_selection::* member, const QString& title,
                    const QString& description, const QString& tag, bool amber);
    void rebuild();
    void refresh_list();
    void show_file(int index);
    void update_counts();
    void update_amber_rows();
    QString pick_path();
    /// Shows the failure dialog. True when the person chose another place.
    bool show_save_failure(const QString& details);
    void saved(const QString& path);

    StudioBackend& backend_;
    const EventLog& log_;
    bundle_selection selection_;
    bool available_ = true;
    bool has_licence_ = false;
    bool names_computers_ = false;
    path_picker picker_;
    folder_opener opener_;
    QVector<bundle_file> files_;
    QVector<row> rows_;

    QListWidget* list_ = nullptr;
    QPlainTextEdit* preview_ = nullptr;
    QLabel* shortened_ = nullptr;
    QLabel* count_ = nullptr;
    QPushButton* copy_ = nullptr;
    QPushButton* cancel_ = nullptr;
    QPushButton* save_ = nullptr;
};

} // namespace vss::gui::support
