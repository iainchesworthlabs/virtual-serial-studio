//
// Copyright (c) 2026 Virtual Serial Studio contributors
//
// SPDX-License-Identifier: GPL-3.0-or-later
//

#include "rendering/render_state.hpp"

#include <vss/core/profile.hpp>

#include <QByteArray>
#include <QCoreApplication>
#include <QGuiApplication>

#include <cstdio>
#include <string>

namespace vss::gui::rendering
{
namespace
{

render_state& state_storage()
{
    static render_state state;
    return state;
}

QString source_text(request_source source)
{
    switch (source)
    {
    case request_source::command_line:
        return QStringLiteral("--render");
    case request_source::environment:
        return QStringLiteral("VSS_RENDER");
    case request_source::setting:
        break;
    }
    return QStringLiteral("setting");
}

void log_state(const render_state& state)
{
    const QByteArray line =
        QStringLiteral("vss_gui: rendering %1 (asked for %2 by %3)%4")
            .arg(state.active == renderer::hardware
                     ? QStringLiteral("in hardware")
                     : QStringLiteral("in software"),
                 mode_key(state.requested), source_text(state.source),
                 state.reason.isEmpty()
                     ? QString()
                     : QStringLiteral(": ") + state.reason)
            .toLocal8Bit();
    std::fprintf(stderr, "%s\n", line.constData());
    VSS_PROFILE_MESSAGE_TEXT(std::string("gui.render_mode ") +
                             line.toStdString());
}

} // namespace

const render_state& current()
{
    return state_storage();
}

void set_state(const render_state& state)
{
    state_storage() = state;
}

render_state initialise(render_mode setting, const QString& environment,
                        const QString& command_line)
{
    VSS_PROFILE_ZONE("gui.render.initialise");
    const auto asked = choose_request(setting, environment, command_line);
    render_state state;
    state.requested = asked.mode;
    state.source = asked.source;
    // The offscreen and minimal platforms have no graphics device to ask for.
    const QString platform = QGuiApplication::platformName();
    const bool headless = platform == QLatin1String("offscreen") ||
                          platform == QLatin1String("minimal");
    state.active = (asked.mode == render_mode::software || headless)
                       ? renderer::software
                       : renderer::hardware;
    set_state(state);
    log_state(state);
    return state;
}

bool confirm_gpu(const gpu_report& gpu)
{
    auto state = current();
    state.gpu = gpu;
    const auto chosen = decide(state.requested, gpu);
    if (chosen.active == renderer::software)
    {
        state.active = renderer::software;
        state.fell_back = true;
        state.reason = chosen.reason;
        set_state(state);
        log_state(state);
        Q_EMIT render_events::instance().fell_back(chosen.reason);
        return false;
    }
    set_state(state);
    log_state(state);
    return true;
}

QString describe()
{
    const auto& state = current();
    if (state.active == renderer::hardware)
    {
        if (!state.gpu.device_created)
            return QCoreApplication::translate("Rendering", "Hardware (GPU)");
        return QCoreApplication::translate("Rendering",
                                           "Hardware (GPU): %1, %2")
            .arg(state.gpu.device, state.gpu.backend);
    }
    if (state.fell_back)
    {
        return QCoreApplication::translate("Rendering",
                                           "Software (CPU), because: %1")
            .arg(state.reason);
    }
    return QCoreApplication::translate("Rendering", "Software (CPU)");
}

void report_gpu_failure(const QString& reason)
{
    auto state = current();
    if (state.active == renderer::software)
        return;
    state.active = renderer::software;
    state.fell_back = true;
    state.reason = reason;
    set_state(state);
    log_state(state);
    Q_EMIT render_events::instance().fell_back(reason);
}

render_events& render_events::instance()
{
    static render_events events;
    return events;
}

} // namespace vss::gui::rendering
