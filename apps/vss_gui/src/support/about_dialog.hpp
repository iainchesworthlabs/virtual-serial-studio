//
// Copyright (c) 2026 Virtual Serial Studio contributors
//
// SPDX-License-Identifier: GPL-3.0-or-later
//

#ifndef VSS_GUI_SUPPORT_ABOUT_DIALOG_HPP
#define VSS_GUI_SUPPORT_ABOUT_DIALOG_HPP

#include "backend/studio_backend.hpp"
#include "support/build_info.hpp"

#include <QDialog>
#include <QString>

class QCheckBox;
class QLabel;
class QPushButton;
class QTabWidget;

namespace vss::gui::updates
{
class UpdateService;
} // namespace vss::gui::updates

namespace vss::gui::support
{

/// About: the version and GPLv3 notice, the copyable build information and
/// environment, the component licences and the font statement. Given the
/// update service it also has the Updates tab, second after About.
class AboutDialog final : public QDialog
{
    Q_OBJECT

public:
    /// The backend tells which edition this is.
    explicit AboutDialog(StudioBackend& backend, QWidget* parent = nullptr);
    /// With `updates`, the dialog has the Updates tab and follows the
    /// service while it is open. Null means no such tab. `updates` outlives
    /// the dialog.
    AboutDialog(StudioBackend& backend, updates::UpdateService* updates,
                QWidget* parent = nullptr);

    /// Where the source of this program is published.
    static QString source_url();

    /// The exact blocks the copy buttons put on the clipboard.
    QString build_information_text() const;
    QString environment_text() const;
    QString diagnostic_text() const;
    QTabWidget* tabs() const noexcept { return tabs_; }

    /// Selects the Updates tab. Does nothing without a service.
    void show_updates_tab();

Q_SIGNALS:
    void bundle_requested();
    /// "Update..." in the Updates tab was pressed.
    void update_requested();

private:
    void build();
    QWidget* build_updates_page();
    /// Reads the service again and shows what it says in the Updates tab.
    void refresh_updates();
    void check_for_updates();
    void copy_build_information();
    void copy_environment();
    void copy_diagnostic_information();
    QWidget* rows_page(const QString& intro,
                       const QVector<environment_row>& rows,
                       const QString& copy_text, void (AboutDialog::*copy)());

    build_info info_;
    QString edition_;
    /// The driver's versions in words, or empty.
    QString driver_text_;
    QTabWidget* tabs_ = nullptr;

    updates::UpdateService* updates_ = nullptr;
    QWidget* updates_page_ = nullptr;
    QLabel* updates_version_ = nullptr;
    QLabel* updates_version_label_ = nullptr;
    QLabel* updates_status_ = nullptr;
    QLabel* updates_checked_ = nullptr;
    QLabel* updates_checked_label_ = nullptr;
    QCheckBox* updates_automatic_ = nullptr;
    QPushButton* updates_check_ = nullptr;
    QPushButton* updates_update_ = nullptr;
    /// The state before the last refresh was a running check.
    bool updates_was_checking_ = false;
    /// "Check now" had the focus when it was pressed, so it has it again when
    /// the check ends.
    bool updates_restore_focus_ = false;
};

} // namespace vss::gui::support

#endif // VSS_GUI_SUPPORT_ABOUT_DIALOG_HPP
