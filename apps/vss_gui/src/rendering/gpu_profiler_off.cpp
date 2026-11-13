//
// Copyright (c) 2026 Virtual Serial Studio contributors
//
// SPDX-License-Identifier: GPL-3.0-or-later
//

// The build without Tracy: no GPU zones.

#include "rendering/gpu_profiler.hpp"

namespace vss::gui::rendering
{

struct gpu_profiler::impl
{
};

gpu_profiler::gpu_profiler() = default;
gpu_profiler::~gpu_profiler() = default;

bool gpu_profiler::recording() const noexcept
{
    return false;
}

void gpu_profiler::frame_done(double) {}

} // namespace vss::gui::rendering
