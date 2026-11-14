//
// Copyright (c) 2026 Virtual Serial Studio contributors
//
// SPDX-License-Identifier: GPL-3.0-or-later
//
// The licence banner at the top of the main window
// (docs/gui-design/docs/screens/09-licence-and-support.md, "Licence notices").
// One notice at a time. It is never a dialog, takes no focus and blocks
// nothing. What a person dismissed is kept as dates in the settings under
// `licence/`, so a notice comes back when the spec says it does.
//

#pragma once

#include "backend/studio_backend.hpp"
#include "support/licence_text.hpp"
#include "widgets/notes.hpp"

#include <QDate>

#include <functional>
#include <optional>

class QPushButton;
class QTimer;

namespace vss::gui
{

class LicenceNotice final : public NoteBanner
{
    Q_OBJECT

public:
    explicit LicenceNotice(StudioBackend& backend, QWidget* parent = nullptr);

    /// The date "today" is for dismissing and reminding. The default is the
    /// local date. A test replaces it.
    void set_today(std::function<QDate()> today);

    /// Reads the licence and the settings again and shows or hides the banner.
    void refresh();

    /// The notice on show, if any.
    std::optional<support::notice_kind> shown() const noexcept
    {
        return shown_kind_;
    }

    /// "Dismiss" or "Remind me tomorrow".
    QPushButton* second_button() const noexcept { return second_; }

    /// Presses the second button's action.
    void dismiss();

Q_SIGNALS:
    /// The banner's "Licence" action was pressed.
    void licence_requested();

private:
    QDate today() const;
    bool due(const support::licence_notice_text& notice,
             const vss::license::status& status);

    StudioBackend& backend_;
    std::function<QDate()> today_;
    QPushButton* second_ = nullptr;
    QTimer* timer_ = nullptr;
    std::optional<support::notice_kind> shown_kind_;
    QString shown_text_;
    /// The licence whose "ended" notice is on show in this run. The notice is
    /// shown once; it stays until dismissed.
    QString expired_open_;
};

} // namespace vss::gui
