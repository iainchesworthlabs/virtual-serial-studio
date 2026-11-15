//
// Copyright (c) 2026 Virtual Serial Studio contributors
//
// SPDX-License-Identifier: GPL-3.0-or-later
//

#include "shell/update_notice.hpp"

#include "support/update_text.hpp"
#include "widgets/common.hpp"

#include <QCoreApplication>
#include <QHBoxLayout>
#include <QPushButton>

namespace vss::gui
{

UpdateNotice::UpdateNotice(updates::UpdateService& service, QWidget* parent)
    : NoteBanner(parent), service_(service)
{
    setObjectName(QStringLiteral("updateNotice"));
    set_kind(NoteBanner::kind::info);
    // The first action is the banner's own button. Its text changes with
    // what is on show, so the click is read through `action_triggered`.
    set_action(QCoreApplication::translate("UpdateNotice", "Turn on"));
    action_button()->setObjectName(QStringLiteral("updateNoticeFirst"));
    connect(this, &NoteBanner::action_triggered, this,
            [this] { first_pressed(); });

    // The second action sits beside the first, in the same style.
    second_ = new QPushButton(this);
    second_->setObjectName(QStringLiteral("updateNoticeSecond"));
    set_role(second_, "quiet");
    second_->setProperty("size", QStringLiteral("small"));
    polish(second_);
    second_->setCursor(Qt::PointingHandCursor);
    if (auto* row = qobject_cast<QHBoxLayout*>(layout()))
        row->addWidget(second_, 0, Qt::AlignVCenter);
    connect(second_, &QPushButton::clicked, this, [this] { second_pressed(); });
    hide();

    connect(&service_, &updates::UpdateService::changed, this,
            [this] { refresh(); });
    refresh();
}

void UpdateNotice::refresh()
{
    content wanted = content::none;
    QString text;
    if (service_.question_due())
    {
        wanted = content::question;
        text = support::update_question_text();
    }
    else if (service_.notice_due() && service_.state().offer.has_value())
    {
        wanted = content::update;
        text =
            support::update_notice_text(service_.state().offer->version_text);
    }
    if (wanted == content::none)
    {
        hide();
        shown_ = content::none;
        shown_text_.clear();
        return;
    }
    // Setting the text announces the banner, so do it once per text.
    if (wanted != shown_ || text != shown_text_)
    {
        const bool question = wanted == content::question;
        set_text(text);
        set_action(
            question ? QCoreApplication::translate("UpdateNotice", "Turn on")
                     : QCoreApplication::translate("UpdateNotice", "Details"));
        second_->setText(
            question ? QCoreApplication::translate("UpdateNotice", "No thanks")
                     : QCoreApplication::translate("UpdateNotice", "Later"));
        second_->setAccessibleName(second_->text());
    }
    shown_ = wanted;
    shown_text_ = text;
    if (isHidden())
        show();
}

void UpdateNotice::first_pressed()
{
    if (shown_ == content::question)
    {
        service_.answer_question(true);
        Q_EMIT answered(true);
    }
    else if (shown_ == content::update)
    {
        Q_EMIT details_requested();
    }
}

void UpdateNotice::second_pressed()
{
    if (shown_ == content::question)
    {
        service_.answer_question(false);
        Q_EMIT answered(false);
    }
    else if (shown_ == content::update)
    {
        service_.hide_notice();
    }
}

} // namespace vss::gui
