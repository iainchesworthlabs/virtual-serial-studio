//
// Copyright (c) 2026 Virtual Serial Studio contributors
//
// SPDX-License-Identifier: GPL-3.0-or-later
//
// The banner below the licence notice that speaks about updates
// (docs/gui-design/docs/screens/12-updates.md, "The question at first start"
// and "Update notice"). It shows one of two things, read from the service:
// the question "Check for updates automatically?" until it is answered, then
// "Virtual Serial Studio <version> is available." when a check on the timer
// found a version. It is never a dialog, takes no focus and blocks nothing.
//

#pragma once

#include "updates/update_service.hpp"
#include "widgets/notes.hpp"

class QPushButton;

namespace vss::gui
{

class UpdateNotice final : public NoteBanner
{
    Q_OBJECT

public:
    /// What the banner says now.
    enum class content
    {
        none,
        /// "Check for updates automatically?" with "Turn on" and "No thanks".
        question,
        /// "Virtual Serial Studio <version> is available." with "Details"
        /// and "Later".
        update
    };

    /// `service` outlives the banner.
    explicit UpdateNotice(updates::UpdateService& service,
                          QWidget* parent = nullptr);

    /// Reads the service again and shows, changes or hides the banner. It is
    /// called on every `changed()` of the service.
    void refresh();

    content shown() const noexcept { return shown_; }

    /// "No thanks" or "Later". The first button is `action_button()`.
    QPushButton* second_button() const noexcept { return second_; }

Q_SIGNALS:
    /// "Turn on" or "No thanks" was pressed and the answer is stored.
    /// `turned_on` is which of the two.
    void answered(bool turned_on);
    /// "Details" was pressed.
    void details_requested();

private:
    void first_pressed();
    void second_pressed();

    updates::UpdateService& service_;
    QPushButton* second_ = nullptr;
    content shown_ = content::none;
    /// The text on show. Setting the text announces the banner, so it is
    /// set only when this changes.
    QString shown_text_;
};

} // namespace vss::gui
