// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include "video_core/renderer_vulkan/present/frame_gen_trace.h"

#include <mutex>
#include <string>

#include <fmt/format.h>

#include "common/logging.h"

namespace Vulkan::FrameGenTrace {
namespace {

struct State {
    std::mutex mutex;
    std::chrono::steady_clock::time_point report_from{std::chrono::steady_clock::now()};
    u64 real_received{};
    u64 generated_recorded{};
    u64 real_presented{};
    u64 generated_presented{};
    u64 dropped{};
    u64 delayed{};
    u64 present_events{};
    u64 queue_samples{};
    double process_cpu_ms{};
    double generation_cpu_ms{};
    double history_copy_gpu_ms{};
    double flow_gpu_ms{};
    double interpolation_gpu_ms{};
    double queue_wait_ms{};
    double ready_lateness_ms{};
    double present_service_ms{};
    double pool_free_sum{};
    size_t pool_capacity{};
    size_t pool_free_last{};
};

State& GetState() {
    static State state;
    return state;
}

void ReportIfDue(std::unique_lock<std::mutex>& lock) {
    State& state = GetState();
    const auto now = std::chrono::steady_clock::now();
    if (now - state.report_from < std::chrono::seconds{1}) {
        return;
    }
    const double seconds = std::chrono::duration<double>(now - state.report_from).count();
    const auto per_second = [seconds](u64 value) { return static_cast<double>(value) / seconds; };
    const auto per_second_ms = [seconds](double value) { return value / seconds; };
    const std::string summary = fmt::format(
        "[FGTRACE] summary real_received={:.1f}/s generated_recorded={:.1f}/s "
        "real_presented={:.1f}/s generated_presented={:.1f}/s dropped={:.1f}/s delayed={:.1f}/s "
        "cpu_process_wall_ms={:.3f}/real_frame cpu_generate_record_ms={:.3f}/generated_frame "
        "history_copy_gpu_ms={:.3f}/s flow_dispatch_gpu_ms={:.3f}/s "
        "interpolation_dispatch_gpu_ms={:.3f}/s fg_scoped_gpu_ms={:.3f}/s "
        "queue_wait_ms={:.3f}/s pool_free_avg={:.1f} pool_free_last={}/{} "
        "ready_lateness_ms={:.3f}/present "
        "present_service_ms={:.3f}/present "
        "repeat_detection=unavailable",
        per_second(state.real_received), per_second(state.generated_recorded),
        per_second(state.real_presented), per_second(state.generated_presented),
        per_second(state.dropped), per_second(state.delayed),
        state.real_received ? state.process_cpu_ms / state.real_received : 0.0,
        state.generated_recorded ? state.generation_cpu_ms / state.generated_recorded : 0.0,
        per_second_ms(state.history_copy_gpu_ms), per_second_ms(state.flow_gpu_ms),
        per_second_ms(state.interpolation_gpu_ms),
        per_second_ms(state.history_copy_gpu_ms + state.flow_gpu_ms +
                  state.interpolation_gpu_ms),
        per_second_ms(state.queue_wait_ms),
        state.queue_samples ? state.pool_free_sum / state.queue_samples : 0.0,
        state.pool_free_last,
        state.pool_capacity,
        state.present_events ? state.ready_lateness_ms / state.present_events : 0.0,
        state.present_events ? state.present_service_ms / state.present_events : 0.0);
    state.real_received = 0;
    state.generated_recorded = 0;
    state.real_presented = 0;
    state.generated_presented = 0;
    state.dropped = 0;
    state.delayed = 0;
    state.present_events = 0;
    state.queue_samples = 0;
    state.process_cpu_ms = 0.0;
    state.generation_cpu_ms = 0.0;
    state.history_copy_gpu_ms = 0.0;
    state.flow_gpu_ms = 0.0;
    state.interpolation_gpu_ms = 0.0;
    state.queue_wait_ms = 0.0;
    state.ready_lateness_ms = 0.0;
    state.present_service_ms = 0.0;
    state.pool_free_sum = 0.0;
    state.report_from = now;
    lock.unlock();
    LOG_INFO(Render_Vulkan, "{}", summary);
}

} // namespace

void QueueState(size_t capacity, size_t free_frames,
                std::chrono::steady_clock::duration blocked) {
    if (!Enabled()) {
        return;
    }
    State& state = GetState();
    std::unique_lock lock{state.mutex};
    ++state.queue_samples;
    state.queue_wait_ms += std::chrono::duration<double, std::milli>(blocked).count();
    state.pool_capacity = capacity;
    state.pool_free_last = free_frames;
    state.pool_free_sum += free_frames;
    ReportIfDue(lock);
}

void RealFrame(u64 frame, double cpu_wall_ms) {
    if (!Enabled()) {
        return;
    }
    State& state = GetState();
    std::unique_lock lock{state.mutex};
    ++state.real_received;
    state.process_cpu_ms += cpu_wall_ms;
    ReportIfDue(lock);
    LOG_INFO(Render_Vulkan, "[FGTRACE] real frame={} t_ms={:.3f} cpu_process_wall_ms={:.3f}", frame,
             TimestampMs(std::chrono::steady_clock::now()), cpu_wall_ms);
}

void GeneratedFrame(u64 frame, u32 ordinal, double cpu_record_ms) {
    if (!Enabled()) {
        return;
    }
    State& state = GetState();
    std::unique_lock lock{state.mutex};
    ++state.generated_recorded;
    state.generation_cpu_ms += cpu_record_ms;
    ReportIfDue(lock);
    LOG_INFO(Render_Vulkan,
             "[FGTRACE] generated frame={} ordinal={} recorded_t_ms={:.3f} cpu_record_ms={:.3f}",
             frame, ordinal, TimestampMs(std::chrono::steady_clock::now()), cpu_record_ms);
}

void GpuWork(double history_copy_ms, double flow_ms, double interpolation_ms) {
    if (!Enabled()) {
        return;
    }
    State& state = GetState();
    std::unique_lock lock{state.mutex};
    state.history_copy_gpu_ms += history_copy_ms;
    state.flow_gpu_ms += flow_ms;
    state.interpolation_gpu_ms += interpolation_ms;
    ReportIfDue(lock);
    LOG_INFO(Render_Vulkan,
             "[FGTRACE] gpu history_copy_ms={:.3f} optical_flow_dispatch_ms={:.3f} "
             "interpolation_dispatch_ms={:.3f} fg_scoped_ms={:.3f}",
             history_copy_ms, flow_ms, interpolation_ms,
             history_copy_ms + flow_ms + interpolation_ms);
}

void Presented(u64 frame, u32 ordinal, bool generated, bool succeeded,
               std::chrono::steady_clock::time_point timestamp, double ready_lateness_ms,
               double present_service_ms) {
    if (!Enabled()) {
        return;
    }
    State& state = GetState();
    std::unique_lock lock{state.mutex};
    ++state.present_events;
    state.ready_lateness_ms += ready_lateness_ms;
    state.present_service_ms += present_service_ms;
    if (succeeded) {
        generated ? ++state.generated_presented : ++state.real_presented;
    } else {
        ++state.dropped;
    }
    if (ready_lateness_ms > 1.0) {
        ++state.delayed;
    }
    ReportIfDue(lock);
    LOG_INFO(Render_Vulkan,
             "[FGTRACE] present frame={} ordinal={} kind={} success={} t_ms={:.3f} "
             "ready_lateness_ms={:.3f} present_service_ms={:.3f}",
             frame, ordinal, generated ? "generated" : "real", succeeded,
             TimestampMs(timestamp), ready_lateness_ms, present_service_ms);
}

} // namespace Vulkan::FrameGenTrace