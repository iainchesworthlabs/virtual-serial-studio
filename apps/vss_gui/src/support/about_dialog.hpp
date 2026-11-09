//
// Copyright (c) 2026 Virtual Serial Studio contributors
//
// SPDX-License-Identifier: GPL-3.0-or-later
//

#ifndef VSS_GUI_SUPPORT_ABOUT_DIALOG_HPP
#define VSS_GUI_SUPPORT_ABOUT_DIALOG_HPP

#include "support/build_info.hpp"

#include <QDialog>
#include <QString>

class QTabWidget;

namespace vss::gui::support
{

/// About: the version and GPLv3 notice, the copyable environment, the
/// component licences and the font statement.
class AboutDialog final : public QDialog
{
    Q_OBJECT

public:
    explicit AboutDialog(QWidget* parent = nullptr);

    /// Where the source of this program is published.
    static QString source_url();

    /// The exact block "Copy environment" puts on the clipboard.
    QString environment_text() const;
    QTabWidget* tabs() const noexcept { return tabs_; }

Q_SIGNALS:
    void bundle_requested();

private:
    void build();
    void copy_environment();

    build_info info_;
    QTabWidget* tabs_ = nullptr;
};

} // namespace vss::gui::support

#endif // VSS_GUI_SUPPORT_ABOUT_DIALOG_HPP
