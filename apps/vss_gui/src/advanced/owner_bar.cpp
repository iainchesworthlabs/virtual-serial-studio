//
// Copyright (c) 2026 Virtual Serial Studio contributors
//
// SPDX-License-Identifier: GPL-3.0-or-later
//

#include "advanced/owner_bar.hpp"
#include "language/bidi.hpp"

#include "advanced/chrome.hpp"
#include "advanced/line_presets.hpp"
#include "widgets/chips.hpp"
#include "widgets/common.hpp"

#include <vss/core/profile.hpp>

#include <QComboBox>
#include <QCoreApplication>
#include <QHBoxLayout>
#include <QLabel>
#include <QPushButton>
#include <QSignalBlocker>
#include <QStringList>
#include <QVBoxLayout>

namespace vss::gui
{

namespace
{

/// The ports that can own the link: its sources on a join, and its targets on
/// a two-way link or a split, where the owner arbitrates the replies.
const QVector<vss::endpoint_id>& owner_candidates(const link_view& link)
{
    return link.mode == vss::link_mode::duplex ? link.targets : link.sources;
}

/// The ports the candidates meet at: the target of a join, and the source of a
/// split.
const QVector<vss::endpoint_id>& meeting_ports(const link_view& link)
{
    return link.mode == vss::link_mode::duplex ? link.sources : link.targets;
}

} // namespace

OwnerBar::OwnerBar(StudioBackend& backend, QWidget* parent)
    : QFrame(parent), backend_(backend)
{
    setObjectName(QStringLiteral("ownerBar"));
    setAttribute(Qt::WA_StyledBackground);
    setAccessibleName(QCoreApplication::translate("OwnerBar", "Owner control"));
    hide();

    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(12, 10, 12, 10);
    layout->setSpacing(8);

    auto* first = new QHBoxLayout();
    first->setContentsMargins(0, 0, 0, 0);
    first->setSpacing(8);
    auto* title = new QLabel(
        QCoreApplication::translate("OwnerBar", "Links on device"), this);
    QFont title_font = Theme::instance().font(text_style::body);
    title_font.setWeight(QFont::DemiBold);
    title->setFont(title_font);
    first->addWidget(title);
    shape_tag_ = new Tag(QCoreApplication::translate("OwnerBar", "Join"),
                         Tag::variant::s, this);
    shape_tag_->setAccessibleName(
        QCoreApplication::translate("OwnerBar", "Join link"));
    first->addWidget(shape_tag_);
    summary_ = new QLabel(this);
    summary_->setFont(Theme::instance().font(text_style::body));
    summary_->setProperty("muted", true);
    summary_->setWordWrap(true);
    first->addWidget(summary_, 1);
    layout->addLayout(first);

    auto* second = new QHBoxLayout();
    second->setContentsMargins(0, 0, 0, 0);
    second->setSpacing(8);
    auto* policy_label = new QLabel(
        QCoreApplication::translate("OwnerBar", "If both write at once"), this);
    policy_label->setFont(Theme::instance().font(text_style::caption));
    policy_label->setProperty("muted", true);
    second->addWidget(policy_label);
    policy_ = new QComboBox(this);
    policy_label->setBuddy(policy_);
    policy_->addItem(
        QCoreApplication::translate("OwnerBar", "One owner at a time"));
    policy_->addItem(
        QCoreApplication::translate("OwnerBar", "Mix bytes as they arrive"));
    policy_->setAccessibleName(
        QCoreApplication::translate("OwnerBar", "If both write at once"));
    // The hub fixes the merge policy when the link is created; changing it
    // would mean rebuilding the link and dropping its traffic.  The control is
    // present, read-only and honest about it.
    degrade(policy_, not_available_text());
    policy_->setAccessibleDescription(QCoreApplication::translate(
        "OwnerBar", "The policy is chosen when the link is created."));
    second->addWidget(policy_);
    second->addStretch(1);
    layout->addLayout(second);

    auto* third = new QHBoxLayout();
    third->setContentsMargins(0, 0, 0, 0);
    third->setSpacing(8);
    owner_label_ =
        new QLabel(QCoreApplication::translate("OwnerBar", "Owner now"), this);
    owner_label_->setFont(Theme::instance().font(text_style::caption));
    owner_label_->setProperty("muted", true);
    third->addWidget(owner_label_);
    owner_chip_ =
        new StatusChip(QCoreApplication::translate("OwnerBar", "Nobody yet"),
                       StatusChip::kind::waiting, this);
    owner_chip_->setAccessibleName(
        QCoreApplication::translate("OwnerBar", "Owner now: nobody"));
    third->addWidget(owner_chip_);
    third->addStretch(1);
    release_ =
        make_button(QCoreApplication::translate("OwnerBar", "Release owner"),
                    nullptr, this);
    release_->setProperty("size", QStringLiteral("small"));
    polish(release_);
    release_->setAccessibleName(
        QCoreApplication::translate("OwnerBar", "Release owner"));
    connect(release_, &QPushButton::clicked, this, &OwnerBar::release_owner);
    third->addWidget(release_);
    give_ = make_button(QString(), nullptr, this);
    give_->setProperty("size", QStringLiteral("small"));
    polish(give_);
    connect(give_, &QPushButton::clicked, this, &OwnerBar::give_owner);
    third->addWidget(give_);
    layout->addLayout(third);

    refresh();
    repaint_on_theme_change(this);
    connect(&Theme::instance(), &Theme::theme_changed, this,
            [this] { refresh(); });
    connect(&backend_, &StudioBackend::links_changed, this,
            [this]
            {
                if (link_)
                    refresh();
            });
}

bool OwnerBar::supports_owner(const link_view& link)
{
    if (link.options.policy != vss::merge_policy::owner ||
        link.mode == vss::link_mode::bus)
        return false;
    return owner_candidates(link).size() >= 2;
}

bool OwnerBar::meets_at(const link_view& link, vss::endpoint_id port)
{
    return meeting_ports(link).contains(port);
}

void OwnerBar::set_link(std::optional<link_view> link)
{
    if (link && !supports_owner(*link))
        link.reset();
    link_ = std::move(link);
    setVisible(link_.has_value());
    refresh();
}

void OwnerBar::clear()
{
    link_.reset();
    hide();
    refresh();
}

std::optional<vss::link_id> OwnerBar::link_id() const
{
    if (!link_)
        return std::nullopt;
    return link_->id;
}

QString OwnerBar::port_name(vss::endpoint_id id) const
{
    if (const auto port = backend_.port(id))
        return isolate_name(port->name);
    return QCoreApplication::translate("OwnerBar", "Port %1")
        .arg(static_cast<qulonglong>(id));
}

QString OwnerBar::summary() const
{
    if (!link_)
        return {};
    QStringList candidates;
    for (const vss::endpoint_id id : owner_candidates(*link_))
        candidates << port_name(id);
    QString meeting;
    for (const vss::endpoint_id id : meeting_ports(*link_))
    {
        if (!meeting.isEmpty())
            meeting += QStringLiteral(", ");
        meeting += port_name(id);
    }
    const QString candidate_text = join_names(candidates);
    if (link_->mode == vss::link_mode::duplex)
        return QCoreApplication::translate("OwnerBar", "%1 to %2")
            .arg(meeting, candidate_text);
    return QCoreApplication::translate("OwnerBar", "%1 into %2")
        .arg(candidate_text, meeting);
}

QString OwnerBar::owner_name() const
{
    if (!link_ || link_->owner == 0)
        return QCoreApplication::translate("OwnerBar", "Nobody yet");
    return port_name(link_->owner);
}

vss::endpoint_id OwnerBar::give_target() const
{
    if (!link_)
        return 0;
    for (const vss::endpoint_id id : owner_candidates(*link_))
    {
        if (id != link_->owner)
            return id;
    }
    return 0;
}

void OwnerBar::refresh()
{
    VSS_PROFILE_ZONE("gui.advanced.owner_bar.refresh");
    const capabilities caps = backend_.caps();
    if (!link_)
        return;
    const auto fresh = backend_.link(link_->id);
    if (!fresh)
    {
        link_.reset();
        hide();
        return;
    }
    link_ = *fresh;

    const bool split = link_->mode == vss::link_mode::duplex;
    shape_tag_->set_text(split
                             ? QCoreApplication::translate("OwnerBar", "Split")
                             : QCoreApplication::translate("OwnerBar", "Join"));
    shape_tag_->setAccessibleName(
        split ? QCoreApplication::translate("OwnerBar", "Split link")
              : QCoreApplication::translate("OwnerBar", "Join link"));
    summary_->setText(summary());
    {
        const QSignalBlocker blocker(policy_);
        policy_->setCurrentIndex(
            link_->options.policy == vss::merge_policy::owner ? 0 : 1);
    }

    const bool has_owner = link_->owner != 0;
    owner_chip_->set_text(owner_name());
    owner_chip_->set_kind(has_owner ? StatusChip::kind::healthy
                                    : StatusChip::kind::waiting);
    owner_chip_->setAccessibleName(
        QCoreApplication::translate("OwnerBar", "Owner now: %1")
            .arg(owner_name()));

    release_->setVisible(true);
    if (caps.owner)
    {
        release_->setEnabled(has_owner);
        release_->setToolTip(
            has_owner ? QString()
                      : QCoreApplication::translate(
                            "OwnerBar", "No port owns this link right now."));
        release_->setAccessibleDescription(QString());
    }
    else
    {
        degrade(release_);
    }

    const vss::endpoint_id target = give_target();
    if (!caps.owner)
    {
        if (target != 0)
            give_->setText(QCoreApplication::translate("OwnerBar", "Give to %1")
                               .arg(port_name(target)));
        give_->setVisible(target != 0);
        give_->setAccessibleName(give_->text());
        degrade(give_);
    }
    else if (target == 0)
    {
        give_->hide();
    }
    else
    {
        give_->setText(QCoreApplication::translate("OwnerBar", "Give to %1")
                           .arg(port_name(target)));
        give_->setAccessibleName(give_->text());
        give_->setEnabled(true);
        give_->setToolTip(QString());
        give_->setAccessibleDescription(QString());
        give_->show();
    }
}

void OwnerBar::release_owner()
{
    VSS_PROFILE_ZONE("gui.advanced.owner_bar.release");
    if (!link_)
        return;
    const auto released = backend_.release_owner(link_->id);
    if (!released)
    {
        Q_EMIT message(QCoreApplication::translate(
                           "OwnerBar", "Could not release the owner: %1")
                           .arg(error_text(released.error())));
        return;
    }
    Q_EMIT owner_changed(link_->id, link_->owner, 0);
    Q_EMIT message(QCoreApplication::translate(
        "OwnerBar", "The owner was released. Any port can write now."));
}

void OwnerBar::give_owner()
{
    VSS_PROFILE_ZONE("gui.advanced.owner_bar.give");
    if (!link_)
        return;
    const vss::endpoint_id target = give_target();
    if (target == 0)
        return;
    const auto changed = backend_.set_owner(link_->id, target);
    if (!changed)
    {
        Q_EMIT message(QCoreApplication::translate(
                           "OwnerBar", "Could not change the owner: %1")
                           .arg(error_text(changed.error())));
        return;
    }
    Q_EMIT owner_changed(link_->id, link_->owner, target);
    Q_EMIT message(
        QCoreApplication::translate("OwnerBar", "The owner is now %1.")
            .arg(port_name(target)));
}

} // namespace vss::gui
