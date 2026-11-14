//
// Copyright (c) 2026 Virtual Serial Studio contributors
//
// SPDX-License-Identifier: GPL-3.0-or-later
//
// The Support page (docs/gui-design/docs/screens/
// 09-licence-and-support.md, "Support"): where to get help and what to send.
// Studio copies addresses to the clipboard. It opens no browser and no mail
// program and sends nothing itself.
//

#pragma once

#include "backend/studio_backend.hpp"
#include "support/build_info.hpp"

#include <vss/license/license.hpp>

#include <QDialog>
#include <QString>

class QFormLayout;
class QLabel;
class QPushButton;

namespace vss::gui
{
class StatusChip;
} // namespace vss::gui

namespace vss::gui::support
{

/// Which of the three cards "Your support" shows.
enum class support_variant
{
    /// The licence includes support and has not ended.
    included,
    /// Community, or a licence without support.
    community,
    /// The licence ended, so support ended with it.
    ended
};

/// The card for a licence status and the answer of
/// `StudioBackend::entitlement("support")`.
support_variant support_variant_of(const vss::license::status& status,
                                   const vss::license::entitlement& support);

class SupportDialog final : public QDialog
{
    Q_OBJECT

public:
    explicit SupportDialog(StudioBackend& backend, QWidget* parent = nullptr);

    support_variant variant() const noexcept { return variant_; }

    /// The lines "Copy" puts on the clipboard.
    QString details_text() const;

    StatusChip* support_chip() const noexcept { return chip_; }
    QLabel* support_sentence() const noexcept { return sentence_; }
    QPushButton* copy_address_button() const noexcept { return address_; }
    QPushButton* copy_number_button() const noexcept { return number_; }
    QPushButton* copy_documents_button() const noexcept { return documents_; }
    QPushButton* copy_issues_button() const noexcept { return issues_; }
    QPushButton* renew_button() const noexcept { return renew_; }
    QPushButton* bundle_button() const noexcept { return bundle_; }
    QPushButton* copy_details_button() const noexcept { return details_; }
    QPushButton* help_button() const noexcept { return help_; }
    QPushButton* troubleshoot_button() const noexcept { return troubleshoot_; }
    QPushButton* close_button() const noexcept { return close_; }

    /// "How to renew": opens the renew dialog.
    void show_renew();

Q_SIGNALS:
    /// "Create support bundle": the caller opens the bundle dialog.
    void bundle_requested();
    /// "Help contents".
    void help_requested();
    /// "Why is nothing arriving?"
    void troubleshoot_requested();

private:
    void build();
    void refresh();
    void copy_with_toast(const QString& text);

    StudioBackend& backend_;
    build_info info_;
    support_variant variant_ = support_variant::community;
    QString licence_id_;

    StatusChip* chip_ = nullptr;
    QLabel* sentence_ = nullptr;
    QPushButton* address_ = nullptr;
    QPushButton* number_ = nullptr;
    QPushButton* documents_ = nullptr;
    QPushButton* issues_ = nullptr;
    QPushButton* renew_ = nullptr;
    QPushButton* bundle_ = nullptr;
    QPushButton* details_ = nullptr;
    QPushButton* help_ = nullptr;
    QPushButton* troubleshoot_ = nullptr;
    QPushButton* close_ = nullptr;
    QFormLayout* form_ = nullptr;
    QLabel* version_ = nullptr;
    QLabel* edition_ = nullptr;
    QLabel* licence_ = nullptr;
    QLabel* system_ = nullptr;
};

} // namespace vss::gui::support
