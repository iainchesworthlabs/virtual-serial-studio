//
// Copyright (c) 2026 Virtual Serial Studio contributors
//
// SPDX-License-Identifier: GPL-3.0-or-later
//

#pragma once

#include "rendering/render_mode.hpp"

#include <QObject>
#include <QString>

namespace vss::gui::rendering
{

/// The renderer the process settled on. It starts as what was asked for and
/// is corrected once the canvas has a graphics device (or fails to get one).
struct render_state
{
    render_mode requested = render_mode::software;
    request_source source = request_source::setting;
    renderer active = renderer::software;
    bool fell_back = false;
    QString reason;
    gpu_report gpu;
};

[[nodiscard]] const render_state& current();

/// Replaces the state. For `initialise` and for tests.
void set_state(const render_state& state);

/// Chooses the starting renderer from the setting, `VSS_RENDER` and
/// `--render=`. Software is final. Hardware and automatic start as hardware
/// and are confirmed by `confirm_gpu` when the canvas has a device. A platform
/// without a display (offscreen, minimal) is software. Writes a line to
/// stderr and a `gui.render_mode` message to the profiler. Call after the
/// QGuiApplication exists and before any window.
render_state initialise(render_mode setting, const QString& environment,
                        const QString& command_line);

/// The canvas set up its graphics device (or could not). Applies `decide`:
/// if the answer is software, switches `current()` and tells the events.
/// Returns true when the canvas should keep drawing on the GPU.
bool confirm_gpu(const gpu_report& gpu);

/// One line for About, Environment and `environment.txt`, for example
/// "Hardware (GPU): NVIDIA GeForce RTX 3080, Direct3D 11".
[[nodiscard]] QString describe();

/// The canvas lost its device while running. Switches to software and tells
/// the events.
void report_gpu_failure(const QString& reason);

/// Tells the rest of the program that the renderer changed after start-up.
class render_events : public QObject
{
    Q_OBJECT

public:
    static render_events& instance();

Q_SIGNALS:
    /// `reason` is for the person, already translated.
    void fell_back(const QString& reason);
};

} // namespace vss::gui::rendering
