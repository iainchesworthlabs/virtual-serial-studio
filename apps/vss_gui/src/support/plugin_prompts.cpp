//
// Copyright (c) 2026 Virtual Serial Studio contributors
//
// SPDX-License-Identifier: GPL-3.0-or-later
//

#include "support/plugin_prompts.hpp"

#include "support/plugin_dialogs.hpp"

#include <QDialog>
#include <QWidget>

#include <algorithm>
#include <memory>
#include <utility>

namespace vss::gui::support
{

PluginPromptQueue::PluginPromptQueue(PluginBackend& backend, QWidget* window,
                                     QObject* parent)
    : QObject(parent), backend_(backend), window_(window)
{
    connect(&backend_, &PluginBackend::secret_requested, this,
            [this](const plugin_secret_request& request)
            {
                entry next;
                next.request = request;
                next.waited.start();
                add(std::move(next));
            });
    connect(&backend_, &PluginBackend::control_requested, this,
            [this](const plugin_control_request& request)
            {
                entry next;
                next.request = request;
                next.waited.start();
                add(std::move(next));
            });
}

PluginPromptQueue::~PluginPromptQueue()
{
    shut_down();
}

void PluginPromptQueue::add(entry next)
{
    if (down_)
    {
        answer_no(next);
        return;
    }
    waiting_.push_back(std::move(next));
    pump();
}

void PluginPromptQueue::answer_no(const entry& next)
{
    if (const auto* secret = std::get_if<plugin_secret_request>(&next.request))
        backend_.answer_secret(secret->id, false);
    else if (const auto* control =
                 std::get_if<plugin_control_request>(&next.request))
        backend_.answer_control(control->id, false);
}

void PluginPromptQueue::pump()
{
    while (current_.isNull() && !waiting_.empty() && !down_)
    {
        entry next = std::move(waiting_.front());
        waiting_.pop_front();
        if (std::holds_alternative<plugin_secret_request>(next.request))
            show_secret(next);
        else
            show_control(next);
    }
}

void PluginPromptQueue::show_secret(entry& next)
{
    auto& request = std::get<plugin_secret_request>(next.request);
    // The time counts from the moment the plugin asked.
    const int remaining =
        request.seconds - static_cast<int>(next.waited.elapsed() / 1000);
    if (remaining <= 0)
    {
        backend_.answer_secret(request.id, false);
        return;
    }
    auto* dialog = new PluginSecretDialog(request, remaining, window_);
    // The code now lives in the dialog only.
    request.text.clear();
    const quint64 id = request.id;
    auto answered = std::make_shared<bool>(false);
    const auto answer = [this, id, answered](bool seen)
    {
        if (*answered)
            return;
        *answered = true;
        backend_.answer_secret(id, seen);
    };
    connect(dialog, &PluginSecretDialog::timed_out_now, this,
            [answer, dialog] { answer(dialog->seen()); });
    connect(dialog, &QDialog::finished, this,
            [this, answer, dialog](int)
            {
                answer(dialog->seen());
                current_.clear();
                dialog->deleteLater();
                pump();
            });
    current_ = dialog;
    dialog->open();
}

void PluginPromptQueue::show_control(entry& next)
{
    const auto& request = std::get<plugin_control_request>(next.request);
    if (next.waited.elapsed() / 1000 >= control_patience_)
    {
        backend_.answer_control(request.id, false);
        return;
    }
    auto* dialog = new PluginControlDialog(request, window_);
    const quint64 id = request.id;
    connect(dialog, &QDialog::finished, this,
            [this, id, dialog](int result)
            {
                backend_.answer_control(id, result == QDialog::Accepted);
                current_.clear();
                dialog->deleteLater();
                pump();
            });
    current_ = dialog;
    dialog->open();
}

void PluginPromptQueue::shut_down()
{
    if (down_)
        return;
    down_ = true;
    for (const entry& each : waiting_)
        answer_no(each);
    waiting_.clear();
    if (!current_.isNull())
    {
        // Closing it answers it: a code is not counted as seen, and a
        // control request is refused.
        QPointer<QDialog> shown = current_;
        if (auto* secret = qobject_cast<PluginSecretDialog*>(shown.data()))
            secret->abandon();
        shown->reject();
    }
}

} // namespace vss::gui::support
