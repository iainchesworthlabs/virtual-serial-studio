//
// Copyright (c) 2026 Virtual Serial Studio contributors
//
// SPDX-License-Identifier: GPL-3.0-or-later
//

#pragma once

#include <memory>

namespace vss::gui::rendering
{

/// Reports how long the GPU spent on a frame as a Tracy GPU zone
/// (`gui.gpu.frame`) while the profiler is recording. CMake compiles either
/// `gpu_profiler_tracy.cpp` (a build with Tracy) or `gpu_profiler_off.cpp`;
/// in the second every call does nothing.
///
/// The time comes from QRhi (`QRhiCommandBuffer::lastCompletedGpuTime`), so it
/// is the same code for Direct3D 11 and 12, Vulkan, Metal and OpenGL. QRhi
/// reports one duration per frame, not a start time, so the zone ends when the
/// duration is read and starts that long before: the length is the GPU's, the
/// position on the timeline is within a frame or two.
class gpu_profiler
{
public:
    gpu_profiler();
    ~gpu_profiler();
    gpu_profiler(const gpu_profiler&) = delete;
    gpu_profiler& operator=(const gpu_profiler&) = delete;

    /// True while the profiler is recording, so the caller can skip asking
    /// the GPU for its timing.
    [[nodiscard]] bool recording() const noexcept;

    /// Records one finished GPU frame of `seconds`.
    void frame_done(double seconds);

private:
    struct impl;
    std::unique_ptr<impl> impl_;
};

} // namespace vss::gui::rendering
