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

class QTabWidget;

namespace vss::gui::support
{

/// About: the version and GPLv3 notice, the copyable build information and
/// environment, the component licences and the font statement.
class AboutDialog final : public QDialog
{
    Q_OBJECT

public:
    /// The backend tells which edition this is.
    explicit AboutDialog(StudioBackend& backend, QWidget* parent = nullptr);

    /// Where the source of this program is published.
    static QString source_url();

    /// The exact blocks the copy buttons put on the clipboard.
    QString build_information_text() const;
    QString environment_text() const;
    QString diagnostic_text() const;
    QTabWidget* tabs() const noexcept { return tabs_; }

Q_SIGNALS:
    void bundle_requested();

private:
    void build();
    void copy_build_information();
    void copy_environment();
    void copy_diagnostic_information();
    QWidget* rows_page(const QString& intro,
                       const QVector<environment_row>& rows,
                       const QString& copy_text, void (AboutDialog::*copy)());

    build_info info_;
    QString edition_;
    QTabWidget* tabs_ = nullptr;
};

} // namespace vss::gui::support

#endif // VSS_GUI_SUPPORT_ABOUT_DIALOG_HPP
