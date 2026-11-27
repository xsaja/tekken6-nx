#pragma once

#include <array>
#include <cstddef>
#include <chrono>
#include <cstdint>
#include <rex/cvar.h>

REXCVAR_DECLARE(bool, tekken_render_census);
REXCVAR_DECLARE(bool, tekken_ffxiii_census);
REXCVAR_DECLARE(bool, tekken_pipeline_trace);

namespace renut::gpu::render_census {

using Clock = std::chrono::steady_clock;

inline bool Enabled() {
  static const bool enabled = REXCVAR_GET(tekken_render_census);
  return enabled;
}

inline bool FfxiiiEnabled() {
  static const bool enabled = REXCVAR_GET(tekken_ffxiii_census);
  return enabled;
}

inline bool PipelineEnabled() {
  static const bool enabled = REXCVAR_GET(tekken_pipeline_trace);
  return enabled;
}

extern thread_local uint64_t item_serial;
void NoteRepeatCandidate(bool same_state_and_range, bool transform_changed);
void NoteTextureLevel(uint64_t bytes, bool tiled, uint64_t ns);
void EndFfxiiiFrame();

inline uint64_t Elapsed(Clock::time_point start) {
  return std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() - start).count();
}

enum class Layout { Bulk, ThreeWords, OtherCopy, Expand };
enum class Proof { Cached, Upload, Stability, Replay, Texture };

struct Totals {
  uint64_t calls = 0, items = 0, ns = 0;
  void Add(uint64_t count, uint64_t elapsed) {
    ++calls;
    items += count;
    ns += elapsed;
  }
};

enum class VertexWork {
  Queued, Up, Whole, Promote, FetchQueued, FetchImmediate, FetchPool, Flush,
  HashStability, HashStreamProof, HashIndexProof, HashFetchProof, HashSeed, Count
};
extern thread_local std::array<Totals, static_cast<size_t>(VertexWork::Count)> vertex_work;
bool TraceVertexWork();
void EndPipelineFrame();

class VertexWorkScope {
public:
  VertexWorkScope(VertexWork kind, uint64_t bytes)
      : start_(TraceVertexWork() ? Clock::now() : Clock::time_point{}), kind_(kind), bytes_(bytes) {}
  VertexWorkScope(const VertexWorkScope &) = delete;
  VertexWorkScope &operator=(const VertexWorkScope &) = delete;
  ~VertexWorkScope() {
    if (start_ != Clock::time_point{})
      vertex_work[static_cast<size_t>(kind_)].Add(bytes_, Elapsed(start_));
  }

private:
  Clock::time_point start_;
  VertexWork kind_;
  uint64_t bytes_;
};

struct Window {
  uint64_t frames = 0;
  Clock::time_point start{};
  std::array<Totals, 4> layouts{};
  std::array<Totals, 5> proofs{};
  std::array<uint64_t, 5> dirty{};
  uint64_t adjacent_pair_vertices = 0;
  Totals conversion, flush, dynamic, clean, eligible;
  uint64_t fills = 0, fetch_bytes = 0, crossings = 0, unprovable = 0, dirty_probes = 0, replaced = 0;
  uint64_t probe_ns = 0;
};

extern thread_local Window window;

// Diagnostic estimates only. Particle ranges are slot-pool bounds, not a draw ID.
void NoteParticlePool(const uint8_t *base, bool long_pool);
// tekken6-nx (2026-11-23): the native skinning jobs' output (sub_82548758 / sub_825481F8), by 4 KiB page, so the
// census can say how much of the per-frame vertex conversion reads skinned vertices. Page bounds, not a draw ID.
void NoteSkinOutput(const uint8_t *base, uint32_t bytes);
void NoteQueueLock(uint64_t ns);

void EndFrame();

}  // namespace renut::gpu::render_census
