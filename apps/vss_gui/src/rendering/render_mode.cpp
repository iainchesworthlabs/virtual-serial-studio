//
// Copyright (c) 2026 Virtual Serial Studio contributors
//
// SPDX-License-Identifier: GPL-3.0-or-later
//

#include "rendering/render_mode.hpp"

#include <QCoreApplication>

namespace vss::gui::rendering
{

QString mode_key(render_mode mode)
{
    switch (mode)
    {
    case render_mode::hardware:
        return QStringLiteral("hardware");
    case render_mode::software:
        return QStringLiteral("software");
    case render_mode::automatic:
        break;
    }
    return QStringLiteral("auto");
}

render_mode mode_from_key(const QString& key, bool* ok)
{
    const QString lower = key.trimmed().toLower();
    if (ok != nullptr)
        *ok = true;
    if (lower == QLatin1String("hardware") || lower == QLatin1String("gpu"))
        return render_mode::hardware;
    if (lower == QLatin1String("software") || lower == QLatin1String("cpu"))
        return render_mode::software;
    if (lower == QLatin1String("auto") || lower == QLatin1String("automatic"))
        return render_mode::automatic;
    if (ok != nullptr)
        *ok = false;
    return render_mode::automatic;
}

request choose_request(render_mode setting, const QString& environment,
                       const QString& command_line)
{
    bool ok = false;
    if (!command_line.isEmpty())
    {
        const auto mode = mode_from_key(command_line, &ok);
        if (ok)
            return {mode, request_source::command_line};
    }
    if (!environment.isEmpty())
    {
        const auto mode = mode_from_key(environment, &ok);
        if (ok)
            return {mode, request_source::environment};
    }
    return {setting, request_source::setting};
}

decision decide(render_mode requested, const gpu_report& gpu)
{
    decision result;
    if (requested == render_mode::software)
        return result;
    result.fell_back = true;
    if (!gpu.device_created)
    {
        result.reason = QCoreApplication::translate(
                            "Rendering",
                            "The graphics system could not be started (%1).")
                            .arg(gpu.error.isEmpty()
                                     ? QCoreApplication::translate(
                                           "Rendering", "no reason given")
                                     : gpu.error);
        return result;
    }
    if (requested == render_mode::automatic && gpu.cpu_device)
    {
        result.reason = QCoreApplication::translate(
                            "Rendering",
                            "The only graphics device found is a software "
                            "renderer (%1).")
                            .arg(gpu.device);
        return result;
    }
    result.active = renderer::hardware;
    result.fell_back = false;
    return result;
}

} // namespace vss::gui::rendering
