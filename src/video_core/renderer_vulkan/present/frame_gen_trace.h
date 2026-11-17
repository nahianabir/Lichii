// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <chrono>
#include <cstdlib>
#include <cstring>

#include "common/common_types.h"

namespace Vulkan::FrameGenTrace {

inline bool Enabled() {
    static const bool enabled = [] {
        const char* value = std::getenv("EDEN_FSR3_FG_TRACE");
        return value && std::strcmp(value, "1") == 0;
    }();
    return enabled;
}

inline double TimestampMs(std::chrono::steady_clock::time_point time) {
    static const auto epoch = std::chrono::steady_clock::now();
    return std::chrono::duration<double, std::milli>(time - epoch).count();
}

void QueueState(size_t capacity, size_t free_frames,
                std::chrono::steady_clock::duration blocked);
void RealFrame(u64 frame, double cpu_wall_ms);
void GeneratedFrame(u64 frame, u32 ordinal, double cpu_record_ms);
void GpuWork(double history_copy_ms, double flow_ms, double interpolation_ms);
void Presented(u64 frame, u32 ordinal, bool generated, bool succeeded,
               std::chrono::steady_clock::time_point timestamp,
               double ready_lateness_ms, double present_service_ms);

} // namespace Vulkan::FrameGenTrace