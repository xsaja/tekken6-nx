/**
 * @file    gpu/log.h
 * @brief   Log category for the native renderer.
 */
#pragma once

#include <atomic>
#include <string>

#include <rex/logging.h>

namespace renut::gpu::log {
inline const rex::LogCategoryId Gpu = rex::RegisterLogCategory("renut_gpu");
// tekken6-nx (2026-11-25): while a banjo_renut_diag_resolve_trace frame runs, RGPU_INFO lines go to stderr too (the
// Switch's rex_stderr.log, which the console's logger does not reach). Set by ResolveTraceFrameStart.
inline std::atomic<bool> g_stderr_mirror{false};
void MirrorToStderr(const std::string &line);  // banjo_renut_tuning.cpp
}

#define RGPU_TRACE(...) REXLOG_CAT_TRACE(::renut::gpu::log::Gpu, __VA_ARGS__)
#define RGPU_DEBUG(...) REXLOG_CAT_DEBUG(::renut::gpu::log::Gpu, __VA_ARGS__)
#define RGPU_INFO(...)                                                                      do {                                                                                        REXLOG_CAT_INFO(::renut::gpu::log::Gpu, __VA_ARGS__);                                     if (::renut::gpu::log::g_stderr_mirror.load(std::memory_order_relaxed))                     ::renut::gpu::log::MirrorToStderr(fmt::format(__VA_ARGS__));                          } while (0)
#define RGPU_WARN(...) REXLOG_CAT_WARN(::renut::gpu::log::Gpu, __VA_ARGS__)
#define RGPU_ERROR(...) REXLOG_CAT_ERROR(::renut::gpu::log::Gpu, __VA_ARGS__)
