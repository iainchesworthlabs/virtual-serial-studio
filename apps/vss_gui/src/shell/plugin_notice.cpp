//
// Copyright (c) 2026 Virtual Serial Studio contributors
//
// SPDX-License-Identifier: GPL-3.0-or-later
//

#include "shell/plugin_notice.hpp"

#include "widgets/common.hpp"

#include <QCoreApplication>
#include <QHBoxLayout>
#include <QPushButton>

#include <algorithm>

namespace vss::gui
{

PluginNotice::PluginNotice(QWidget* parent) : NoteBanner(parent)
{
    setObjectName(QStringLiteral("pluginNotice"));
    // What a plugin is called is text it gave: never markup.
    set_text_format(Qt::PlainText);
    set_action(QString(),
               [this]
               {
                   if (!shown_)
                       return;
                   if (shown_->action_decides)
                       Q_EMIT decision_requested(shown_->plugin_id,
                                                 shown_->folder);
                   else
                       Q_EMIT plugins_requested();
               });

    auto add_button = [this](const char* name)
    {
        auto* button = new QPushButton(this);
        button->setObjectName(QString::fromLatin1(name));
        set_role(button, "quiet");
        button->setProperty("size", QStringLiteral("small"));
        polish(button);
        button->setCursor(Qt::PointingHandCursor);
        if (auto* row = qobject_cast<QHBoxLayout*>(layout()))
            row->addWidget(button, 0, Qt::AlignVCenter);
        return button;
    };
    second_ = add_button("pluginNoticeSecond");
    connect(second_, &QPushButton::clicked, this,
            [this]
            {
                if (shown_)
                    Q_EMIT start_again_requested(shown_->plugin_id);
            });
    dismiss_ = add_button("pluginNoticeDismiss");
    dismiss_->setText(QCoreApplication::translate("PluginNotice", "Dismiss"));
    connect(dismiss_, &QPushButton::clicked, this, [this] { dismiss(); });
    // Never takes focus when it appears.
    setFocusPolicy(Qt::NoFocus);
    hide();
}

void PluginNotice::set_backend(PluginBackend* host)
{
    host_ = host;
    before_ = host_ != nullptr ? host_->snapshot() : nullptr;
    if (host_ != nullptr)
        connect(host_, &PluginBackend::snapshot_changed, this,
                [this] { refresh(); });
}

void PluginNotice::refresh()
{
    if (host_ == nullptr)
        return;
    const plugin_snapshot_ptr after = host_->snapshot();
    if (!after)
        return;
    if (before_)
    {
        for (support::plugin_notice_text& each :
             support::plugin_notices(*before_, *after, used_))
        {
            // The same thing is never said twice in a row.
            const bool already =
                (shown_ && shown_->kind == each.kind &&
                 shown_->plugin_id == each.plugin_id) ||
                std::any_of(queue_.begin(), queue_.end(),
                            [&each](const support::plugin_notice_text& other)
                            {
                                return other.kind == each.kind &&
                                       other.plugin_id == each.plugin_id;
                            });
            if (!already)
                queue_.push_back(std::move(each));
        }
    }
    before_ = after;
    if (!shown_)
        show_next();
}

void PluginNotice::dismiss()
{
    shown_.reset();
    hide();
    show_next();
}

void PluginNotice::show_next()
{
    if (queue_.empty())
        return;
    const support::plugin_notice_text notice = std::move(queue_.front());
    queue_.pop_front();
    shown_ = notice;
    set_kind(notice.warn ? NoteBanner::kind::warn : NoteBanner::kind::info);
    set_text(notice.text);
    if (action_button() != nullptr)
    {
        action_button()->setText(notice.action);
        action_button()->setAccessibleName(notice.action);
    }
    second_->setText(notice.second);
    second_->setVisible(!notice.second.isEmpty());
    // Shown without taking the focus.
    show();
}

} // namespace vss::gui
