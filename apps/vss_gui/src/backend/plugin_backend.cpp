//
// Copyright (c) 2026 Virtual Serial Studio contributors
//
// SPDX-License-Identifier: GPL-3.0-or-later
//

#include "backend/plugin_backend.hpp"

#include <QMetaObject>

namespace vss::gui
{

PluginBackend::PluginBackend(QObject* parent) : QObject(parent)
{
    qRegisterMetaType<vss::gui::plugin_request_result>();
    qRegisterMetaType<vss::gui::plugin_key_file_view>();
    qRegisterMetaType<vss::gui::plugin_inspection_view>();
}

PluginBackend::~PluginBackend() = default;

void PluginBackend::notify_snapshot_changed()
{
    // One queued call is enough for any number of changes before it runs.
    if (snapshot_pending_.exchange(true))
        return;
    QMetaObject::invokeMethod(
        this,
        [this]
        {
            snapshot_pending_.store(false);
            Q_EMIT snapshot_changed();
        },
        Qt::QueuedConnection);
}

void PluginBackend::notify_request_finished(const plugin_request_result& result)
{
    QMetaObject::invokeMethod(
        this, [this, result] { Q_EMIT request_finished(result); },
        Qt::QueuedConnection);
}

void PluginBackend::notify_key_file_read(quint64 request,
                                         const plugin_key_file_view& key)
{
    QMetaObject::invokeMethod(
        this, [this, request, key] { Q_EMIT key_file_read(request, key); },
        Qt::QueuedConnection);
}

void PluginBackend::notify_plugin_inspected(quint64 request,
                                            const plugin_inspection_view& found)
{
    QMetaObject::invokeMethod(
        this, [this, request, found]
        { Q_EMIT plugin_inspected(request, found); }, Qt::QueuedConnection);
}

} // namespace vss::gui
