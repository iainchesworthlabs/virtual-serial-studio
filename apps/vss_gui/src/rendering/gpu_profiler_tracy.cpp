//
// Copyright (c) 2026 Virtual Serial Studio contributors
//
// SPDX-License-Identifier: GPL-3.0-or-later
//

// GPU zones for the canvas, through Tracy's C API for custom GPU contexts.
// Tracy's own backends (OpenGL, Direct3D, Vulkan) each need that API's
// handles; QRhi hides them, and gives the GPU time of a frame directly.

#include "rendering/gpu_profiler.hpp"

#include <vss/core/profile.hpp>

#include <tracy/TracyC.h>
// For NextGpuContextId, which the C API has no counterpart of.
#include <client/TracyProfiler.hpp>

#include <chrono>
#include <cstdint>

namespace vss::gui::rendering
{
namespace
{

constexpr std::uint8_t kCustomContext = 7; // tracy::GpuContextType::Custom

std::int64_t now_ns() noexcept
{
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

} // namespace

struct gpu_profiler::impl
{
    bool context_made = false;
    std::uint8_t context = 0;
    std::int64_t anchor_ns = 0;
    std::uint16_t next_query = 0;
};

gpu_profiler::gpu_profiler() : impl_(std::make_unique<impl>()) {}
gpu_profiler::~gpu_profiler() = default;

bool gpu_profiler::recording() const noexcept
{
    return vss::profiling::detail::armed.load(std::memory_order_relaxed);
}

void gpu_profiler::frame_done(double seconds)
{
    if (!recording() || seconds <= 0.0)
        return;
    const vss::profiling::detail::section section;
    if (!section.active())
        return;
    auto& state = *impl_;
    if (!state.context_made)
    {
        state.context = static_cast<std::uint8_t>(tracy::NextGpuContextId());
        state.anchor_ns = now_ns();
        // The GPU clock here is nanoseconds since this moment, which is also
        // the moment Tracy stamps on the context.
        ___tracy_emit_gpu_new_context_serial(
            ___tracy_gpu_new_context_data{0, 1.0F, state.context, 0,
                                          kCustomContext});
        static constexpr char name[] = "gui.canvas";
        ___tracy_emit_gpu_context_name_serial(
            ___tracy_gpu_context_name_data{state.context, name,
                                           sizeof(name) - 1});
        state.context_made = true;
    }
    const std::int64_t end = now_ns() - state.anchor_ns;
    const auto length = static_cast<std::int64_t>(seconds * 1e9);
    static constexpr char zone_name[] = "gui.gpu.frame";
    static constexpr char function[] = "gpu_profiler::frame_done";
    static constexpr char file[] = __FILE__;
    const std::uint64_t location = ___tracy_alloc_srcloc_name(
        static_cast<std::uint32_t>(__LINE__), file, sizeof(file) - 1, function,
        sizeof(function) - 1, zone_name, sizeof(zone_name) - 1, 0);
    const std::uint16_t begin_query = state.next_query++;
    const std::uint16_t end_query = state.next_query++;
    ___tracy_emit_gpu_zone_begin_alloc_serial(
        ___tracy_gpu_zone_begin_data{location, begin_query, state.context});
    ___tracy_emit_gpu_time_serial(
        ___tracy_gpu_time_data{end - length, begin_query, state.context});
    ___tracy_emit_gpu_zone_end_serial(
        ___tracy_gpu_zone_end_data{end_query, state.context});
    ___tracy_emit_gpu_time_serial(
        ___tracy_gpu_time_data{end, end_query, state.context});
}

} // namespace vss::gui::rendering
