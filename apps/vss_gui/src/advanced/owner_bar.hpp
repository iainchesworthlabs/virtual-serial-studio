//
// Copyright (c) 2026 Virtual Serial Studio contributors
//
// SPDX-License-Identifier: GPL-3.0-or-later
//

#ifndef VSS_GUI_ADVANCED_OWNER_BAR_HPP
#define VSS_GUI_ADVANCED_OWNER_BAR_HPP

#include "backend/studio_backend.hpp"

#include <QFrame>
#include <QString>

#include <optional>

class QComboBox;
class QLabel;
class QPushButton;

namespace vss::gui
{

class StatusChip;
class Tag;

/// The link bar under the topology diagram for an owner-policy join or split.
///
/// It renders "Links on device", the policy, the current owner and the two
/// owner actions.  Owner changes go to the engine through
/// `StudioBackend::set_owner` and `release_owner`; the bar never guesses who
/// owns the link.
class OwnerBar final : public QFrame
{
    Q_OBJECT

public:
    explicit OwnerBar(StudioBackend& backend, QWidget* parent = nullptr);

    /// True when this link has the owner policy and more than one port that
    /// can own it: a join with several sources, or a split with several
    /// targets (the owner of a link that carries replies is one of its
    /// targets).  The only links the owner bar applies to.
    static bool supports_owner(const link_view& link);

    /// True when `port` is the one the candidates meet at: the target of a
    /// join, or the source of a split.
    static bool meets_at(const link_view& link, vss::endpoint_id port);

    /// Shows the given link, or hides the bar when there is none.
    void set_link(std::optional<link_view> link);
    void clear();
    bool has_link() const noexcept { return link_.has_value(); }
    std::optional<vss::link_id> link_id() const;

    QString summary() const;
    QString owner_name() const;
    QPushButton* release_button() const noexcept { return release_; }
    QPushButton* give_button() const noexcept { return give_; }
    QComboBox* policy_combo() const noexcept { return policy_; }

Q_SIGNALS:
    /// A one-line sentence for the status bar.
    void message(const QString& text);
    /// The owner of a link changed; zero means none.  For the Undo history.
    void owner_changed(vss::link_id link, vss::endpoint_id before,
                       vss::endpoint_id after);

private:
    void refresh();
    void release_owner();
    void give_owner();
    vss::endpoint_id give_target() const;
    QString port_name(vss::endpoint_id id) const;

    StudioBackend& backend_;
    std::optional<link_view> link_;

    QLabel* summary_ = nullptr;
    Tag* shape_tag_ = nullptr;
    QComboBox* policy_ = nullptr;
    QLabel* owner_label_ = nullptr;
    StatusChip* owner_chip_ = nullptr;
    QPushButton* release_ = nullptr;
    QPushButton* give_ = nullptr;
};

} // namespace vss::gui

#endif // VSS_GUI_ADVANCED_OWNER_BAR_HPP
