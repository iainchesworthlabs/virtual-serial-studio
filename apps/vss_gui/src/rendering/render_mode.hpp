//
// Copyright (c) 2026 Virtual Serial Studio contributors
//
// SPDX-License-Identifier: GPL-3.0-or-later
//

#pragma once

#include <QString>

namespace vss::gui::rendering
{

/// What the person asked for: Settings, Performance, Rendering
/// (`ui/renderMode`), `VSS_RENDER` or `--render=`.
enum class render_mode
{
    automatic,
    hardware,
    software
};

/// What is actually drawing the canvas.
enum class renderer
{
    hardware,
    software
};

/// Where the request came from. The command line wins over the environment,
/// which wins over the saved setting.
enum class request_source
{
    setting,
    environment,
    command_line
};

/// The key stored in QSettings and accepted by `VSS_RENDER` and `--render=`.
[[nodiscard]] QString mode_key(render_mode mode);

/// Maps a key to a mode. `ok` (if given) is false for an unknown key, and the
/// result is then `automatic`.
[[nodiscard]] render_mode mode_from_key(const QString& key,
                                        bool* ok = nullptr);

struct request
{
    render_mode mode = render_mode::automatic;
    request_source source = request_source::setting;
};

/// Picks the request from the three places it can come from. An empty or
/// unknown environment or command-line value is ignored.
[[nodiscard]] request choose_request(render_mode setting,
                                     const QString& environment,
                                     const QString& command_line);

/// What the canvas found when it set up the GPU: the QRhi device behind the
/// QCanvasPainterWidget. Filled in by `GpuCanvas`, or by a test that wants to
/// exercise `decide`.
struct gpu_report
{
    /// A rendering device exists.
    bool device_created = false;
    /// The graphics API in use, for example "Direct3D 11" or "Metal".
    QString backend;
    /// The adapter, for example "NVIDIA GeForce RTX 3080".
    QString device;
    /// The adapter is a CPU renderer (WARP, llvmpipe), which is slower than
    /// the plain raster path.
    bool cpu_device = false;
    /// Why no device could be made, when `device_created` is false.
    QString error;
};

struct decision
{
    renderer active = renderer::software;
    /// True when the GPU was wanted (hardware, or automatic) and the answer
    /// is software.
    bool fell_back = false;
    /// Why, in a sentence the person can read. Empty when nothing needs saying.
    QString reason;
};

/// The whole fallback rule, without touching the machine:
///   software: software.
///   hardware or automatic: hardware when a device exists.
///   automatic also refuses a device that is itself a CPU renderer; asking for
///   hardware explicitly accepts it, for diagnosis.
[[nodiscard]] decision decide(render_mode requested, const gpu_report& gpu);

} // namespace vss::gui::rendering
