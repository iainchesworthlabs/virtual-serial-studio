//
// Copyright (c) 2026 Virtual Serial Studio contributors
//
// SPDX-License-Identifier: GPL-3.0-or-later
//
// Shows what a running plugin asks Studio to put in front of the user: a code
// to read once and a control session to allow or refuse. One dialog at a time;
// a second request waits its turn. Every request gets one answer, also when
// the window is closing.
//

#pragma once

#include "backend/plugin_backend.hpp"

#include <QElapsedTimer>
#include <QObject>
#include <QPointer>

#include <deque>
#include <variant>

class QDialog;
class QWidget;

namespace vss::gui::support
{

class PluginPromptQueue final : public QObject
{
    Q_OBJECT

public:
    /// Listens to `backend`, and shows its dialogs over `window`.
    PluginPromptQueue(PluginBackend& backend, QWidget* window,
                      QObject* parent = nullptr);
    ~PluginPromptQueue() override;

    /// The dialog on the screen, or null.
    QDialog* current_dialog() const noexcept { return current_; }
    /// How many requests wait behind it.
    std::size_t waiting() const noexcept { return waiting_.size(); }
    /// How long a control request may wait before it is answered no without
    /// being shown, in seconds. The plugin stops waiting after about this
    /// long.
    void set_control_patience(int seconds) { control_patience_ = seconds; }

    /// The window is closing: everything waiting is answered no and the
    /// dialog on the screen is closed. Nothing waits on a dialog.
    void shut_down();

private:
    struct entry
    {
        std::variant<plugin_secret_request, plugin_control_request> request;
        QElapsedTimer waited;
    };

    void add(entry next);
    void pump();
    void show_secret(entry& next);
    void show_control(entry& next);
    void answer_no(const entry& next);

    PluginBackend& backend_;
    QPointer<QWidget> window_;
    QPointer<QDialog> current_;
    std::deque<entry> waiting_;
    int control_patience_ = 120;
    bool down_ = false;
};

} // namespace vss::gui::support
