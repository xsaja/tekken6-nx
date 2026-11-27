// Frame-chain diagnostic (rex/frame_chain.h). tekken6-nx 2026-11-20.
#include <rex/frame_chain.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cstdio>

#include <rex/cvar.h>

REXCVAR_DEFINE_BOOL(tekken_frame_chain, false, "Tekken",
                    "Frame-chain diagnostic: every 10 s two '[frame_chain]' lines on stderr with, per frame (one "
                    "Swap of the guest main thread), the main thread's time in the Swap, in each kind of kernel wait "
                    "and signal, the rest (busy), the three objects it waited on longest, and the render thread's "
                    "busy / idle / swapchain / fence / lock time (docs/switch-port-log.md)")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);

REXCVAR_DEFINE_BOOL(tekken_frame_chain_slow, false, "Tekken",
                    "Report the slowest frame of at least 25 ms and up to two more long stalls per window")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);

namespace rex::frame_chain {

#if defined(__cpp_constinit)
constinit thread_local bool t_main = false;
#else
thread_local bool t_main = false;
#endif
#if defined(_WIN32)
bool OnMain() { return t_main; }
#endif

namespace {

constexpr uint64_t kReportNs = 10'000'000'000ull;
constexpr uint32_t kMaxFrameSamples = 4096;  // 10 s at up to ~400 fps
constexpr uint32_t kObjects = 64;

struct ObjectStat {
  uint32_t key;
  uint8_t type;
  uint32_t count;
  uint64_t ns;
};

struct SlowWait {
  uint64_t ns = 0;
  uint32_t object = 0;
  Wait kind = kWaitObj;
  uint8_t type = kObjOther;
};

// One atomic word keeps the count, sum and slowest level in the same
// interval when ReadLevel completes concurrently with SwapEnd.
struct TextureLevels {
  static constexpr uint64_t kNsPerTick = 32000;
  static constexpr uint64_t kBytesPerPage = 4096;
  static constexpr unsigned kTotalBits = 20, kCountBits = 12, kPeakBits = 16;
  static constexpr unsigned kCountShift = kTotalBits, kPeakShift = kCountShift + kCountBits;
  static constexpr unsigned kSizeShift = kPeakShift + kPeakBits;
  static constexpr uint64_t kTotalMax = (uint64_t{1} << kTotalBits) - 1;
  static constexpr uint64_t kCountMax = (uint64_t{1} << kCountBits) - 1;
  static constexpr uint64_t kPeakMax = (uint64_t{1} << kPeakBits) - 1;

  uint64_t total_ticks = 0, levels = 0, peak_ticks = 0, peak_pages = 0;

  static TextureLevels Unpack(uint64_t word) {
    return {word & kTotalMax, (word >> kCountShift) & kCountMax,
            (word >> kPeakShift) & kPeakMax, word >> kSizeShift};
  }

  uint64_t Pack() const {
    return total_ticks | (levels << kCountShift) | (peak_ticks << kPeakShift) | (peak_pages << kSizeShift);
  }

  void Add(uint64_t bytes, uint64_t ns) {
    const uint64_t ticks = ns / kNsPerTick + (ns % kNsPerTick >= kNsPerTick / 2);
    const uint64_t pages = bytes / kBytesPerPage + (bytes % kBytesPerPage >= kBytesPerPage / 2);
    total_ticks = std::min(total_ticks + ticks, kTotalMax);
    levels = std::min(levels + 1, kCountMax);
    const uint64_t peak = std::min(ticks, kPeakMax);
    if (peak > peak_ticks || (peak == peak_ticks && pages > peak_pages)) {
      peak_ticks = peak;
      peak_pages = std::min(pages, kPeakMax);
    }
  }
};
std::atomic<uint64_t> g_texture_levels{0};

struct SlowFrame {
  uint64_t index = 0, ns = 0, swap = 0, ahead = 0;
  uint64_t waits = 0, render = 0, fills = 0;
  TextureLevels textures;
  SlowWait longest;
};

// The main thread's accumulators (only the main thread touches them).
struct MainState {
  uint32_t depth = 0;  // measured scopes open (the Swap included)
  uint64_t swap_start = 0;
  uint64_t last_end = 0;      // the end of the previous Swap (the frame boundary)
  uint64_t window_start = 0;  // 0: no frame closed yet
  uint64_t frames = 0;
  uint64_t frame_ns = 0;
  uint64_t frame_max_ns = 0;
  uint64_t swap_ns = 0;
  uint64_t ahead_ns = 0;
  uint64_t wait_ns[kWaitKinds] = {};
  uint64_t wait_n[kWaitKinds] = {};
  uint32_t samples_n = 0;
  uint32_t samples_us[kMaxFrameSamples] = {};
  ObjectStat objects[kObjects] = {};
  uint32_t objects_n = 0;
  uint64_t objects_overflow_ns = 0;
  bool track_slow = false;
  SlowWait longest_wait;
  std::array<SlowFrame, 3> slowest{};
  uint64_t previous_waits = 0, previous_render = 0, previous_fills = 0;
  uint64_t previous_ahead = 0;
};
MainState g_main;

std::atomic<uint64_t> g_render_ns[kRenderKinds];
std::atomic<uint64_t> g_render_n[kRenderKinds];

bool g_warned_not_main = false;

// tekken_hang_snapshot: written by the Swap hook only (one thread), read by the hang watchdog.
std::atomic<uint64_t> g_last_swap_ns{0};
std::atomic<uint64_t> g_swap_count{0};

void ResetWindow(MainState& m, uint64_t now) {
  m.window_start = now;
  m.frames = m.frame_ns = m.frame_max_ns = m.swap_ns = m.ahead_ns = 0;
  for (uint32_t i = 0; i < kWaitKinds; ++i) m.wait_ns[i] = m.wait_n[i] = 0;
  m.samples_n = 0;
  m.objects_n = 0;
  m.objects_overflow_ns = 0;
  m.slowest = {};
  m.longest_wait = {};
  m.previous_waits = m.previous_render = m.previous_fills = m.previous_ahead = 0;
  for (uint32_t i = 0; i < kRenderKinds; ++i) {
    g_render_ns[i].store(0, std::memory_order_relaxed);
    g_render_n[i].store(0, std::memory_order_relaxed);
  }
}

void NoteObject(MainState& m, uint32_t key, uint8_t type, uint64_t ns) {
  for (uint32_t i = 0; i < m.objects_n; ++i) {
    ObjectStat& o = m.objects[i];
    if (o.key == key && o.type == type) {
      ++o.count;
      o.ns += ns;
      return;
    }
  }
  if (m.objects_n < kObjects) {
    m.objects[m.objects_n++] = {key, type, 1, ns};
  } else {
    m.objects_overflow_ns += ns;
  }
}

const char* TypeName(uint8_t type) {
  switch (type) {
    case kObjEvent: return "event";
    case kObjSemaphore: return "sem";
    case kObjMutant: return "mutant";
    case kObjThread: return "thread";
    case kObjTimer: return "timer";
    case kObjCritSec: return "cs";
    default: return "obj";
  }
}

constexpr const char* kWaitNames[kWaitKinds] = {"obj", "multi", "sigwait", "pulse", "set",
                                                 "sem", "cs",    "sleep",   "rt"};

void NoteSlowFrame(MainState& m, uint64_t frame, uint64_t swap) {
  uint64_t waits = 0;
  for (uint64_t ns : m.wait_ns)
    waits += ns;
  const auto render = g_render_ns[kRenderRun].load(std::memory_order_relaxed);
  const auto fills = g_render_ns[kRenderVertexFills].load(std::memory_order_relaxed);
  const auto textures = TextureLevels::Unpack(g_texture_levels.exchange(0, std::memory_order_relaxed));
  if (frame >= 25'000'000) {
    // Keep the worst ordinary frame, plus up to two more if they were stalls.
    const size_t limit = frame >= 100'000'000 ? m.slowest.size() : 1;
    for (size_t i = 0; i < limit; ++i) {
      if (frame <= m.slowest[i].ns)
        continue;
      for (size_t j = limit - 1; j > i; --j)
        m.slowest[j] = m.slowest[j - 1];
      m.slowest[i] = {SwapCount(), frame, swap, m.ahead_ns - m.previous_ahead,
                      waits - m.previous_waits, render - m.previous_render, fills - m.previous_fills,
                      textures, m.longest_wait};
      break;
    }
  }
  m.longest_wait = {};
  m.previous_waits = waits;
  m.previous_render = render;
  m.previous_fills = fills;
  m.previous_ahead = m.ahead_ns;
}

void Report(MainState& m, uint64_t now) {
  const double frames = double(m.frames);
  const double secs = double(now - m.window_start) / 1e9;
  auto per = [frames](uint64_t ns) { return double(ns) / 1e6 / frames; };
  uint64_t waits = 0;
  for (uint32_t i = 0; i < kWaitKinds; ++i) waits += m.wait_ns[i];
  const uint64_t accounted = m.swap_ns + waits;
  const double busy = m.frame_ns > accounted ? per(m.frame_ns - accounted) : 0.0;

  // p90 of the frame times (in place: the samples are rebuilt next window).
  double p90 = 0.0;
  if (m.samples_n) {
    const uint32_t k = std::min(m.samples_n - 1, uint32_t(double(m.samples_n) * 0.9));
    std::nth_element(m.samples_us, m.samples_us + k, m.samples_us + m.samples_n);
    p90 = double(m.samples_us[k]) / 1000.0;
  }

  char line[1024];
  int len = std::snprintf(line, sizeof(line),
                          "[frame_chain] %.1f fps | frame %.1f ms (p90 %.1f, max %.1f) | main: busy %.2f, swap %.2f "
                          "(ahead %.2f), waits:",
                          frames / secs, per(m.frame_ns), p90, double(m.frame_max_ns) / 1e6, busy, per(m.swap_ns),
                          per(m.ahead_ns));
  for (uint32_t i = 0; i < kWaitKinds && len > 0 && len < int(sizeof(line)); ++i) {
    len += std::snprintf(line + len, sizeof(line) - size_t(len), " %s %.2f/%.1fx", kWaitNames[i], per(m.wait_ns[i]),
                         double(m.wait_n[i]) / frames);
  }
  std::fprintf(stderr, "%s | %llu frames in %.1f s\n", line, (unsigned long long)m.frames, secs);

  // Top 3 objects by wait time.
  const uint32_t top = std::min<uint32_t>(3, m.objects_n);
  std::partial_sort(m.objects, m.objects + top, m.objects + m.objects_n,
                    [](const ObjectStat& a, const ObjectStat& b) { return a.ns > b.ns; });
  len = std::snprintf(line, sizeof(line), "[frame_chain] top obj:");
  for (uint32_t i = 0; i < top && len > 0 && len < int(sizeof(line)); ++i) {
    const ObjectStat& o = m.objects[i];
    len += std::snprintf(line + len, sizeof(line) - size_t(len), "%s 0x%08X %s %.2f ms x %.1f/frame", i ? "," : "",
                         o.key, TypeName(o.type), per(o.ns), double(o.count) / frames);
  }
  if (!top && len > 0 && len < int(sizeof(line)))
    len += std::snprintf(line + len, sizeof(line) - size_t(len), " none");
  if (m.objects_overflow_ns && len > 0 && len < int(sizeof(line)))
    len += std::snprintf(line + len, sizeof(line) - size_t(len), " (+%.2f ms beyond %u objects)",
                         per(m.objects_overflow_ns), kObjects);

  uint64_t r[kRenderKinds];
  for (uint32_t i = 0; i < kRenderKinds; ++i) r[i] = g_render_ns[i].load(std::memory_order_relaxed);
  const uint64_t inner = r[kRenderSwapchain] + r[kRenderFence] + r[kRenderOtherWait] + r[kRenderLock];
  const uint64_t rbusy = r[kRenderRun] > inner ? r[kRenderRun] - inner : 0;
  std::fprintf(stderr,
               "%s | render: busy %.2f, wait-main %.2f, swapchain %.2f, fence %.2f, other-wait %.2f, lock %.2f "
               "(%.1f batches/frame)\n",
               line, per(rbusy), per(r[kRenderIdle]), per(r[kRenderSwapchain]), per(r[kRenderFence]),
               per(r[kRenderOtherWait]), per(r[kRenderLock]),
               double(g_render_n[kRenderRun].load(std::memory_order_relaxed)) / frames);
  const auto ms = [](uint64_t ns) { return double(ns) / 1e6; };
  for (size_t i = 0; i < m.slowest.size(); ++i) {
    const auto &slow = m.slowest[i];
    if (!slow.ns || (i && slow.ns < 100'000'000))
      break;
    char wait[92] = "none";
    if (slow.longest.ns) {
      std::snprintf(wait, sizeof(wait), "%s %s 0x%08X %.2f ms",
                    kWaitNames[slow.longest.kind], TypeName(slow.longest.type),
                    slow.longest.object, ms(slow.longest.ns));
    }
    std::fprintf(stderr, "[frame_slow] swap #%llu %.2f ms | main waits %.2f swap %.2f ahead %.2f"
                         " | render batches %.2f vertex fills %.2f ms completed between swaps"
                         " | ReadLevel %.2f ms / %llu levels, slowest %.2f ms (%.3f MiB) | longest wait %s\n",
                 static_cast<unsigned long long>(slow.index), ms(slow.ns), ms(slow.waits),
                 ms(slow.swap), ms(slow.ahead), ms(slow.render), ms(slow.fills),
                 ms(slow.textures.total_ticks * TextureLevels::kNsPerTick),
                 static_cast<unsigned long long>(slow.textures.levels),
                 ms(slow.textures.peak_ticks * TextureLevels::kNsPerTick),
                 double(slow.textures.peak_pages * TextureLevels::kBytesPerPage) / 1048576.0, wait);
  }
  std::fflush(stderr);
}

}  // namespace

bool Enabled() {
  static const bool on = REXCVAR_GET(tekken_frame_chain);
  return on;
}

bool SlowFramesEnabled() {
  static const bool on = Enabled() && REXCVAR_GET(tekken_frame_chain_slow);
  return on;
}

void MarkMainThread() {
  if (!Enabled()) return;
  t_main = true;
  g_main.track_slow = SlowFramesEnabled();
  std::fprintf(stderr, "[frame_chain] on: measuring the guest main thread (report every 10 s)\n");
  std::fflush(stderr);
}

bool EnterMain() { return g_main.depth++ == 0; }

void LeaveMain(Wait kind, uint64_t ns, uint32_t object_key, uint8_t object_type, bool counted) {
  MainState& m = g_main;
  if (m.depth) --m.depth;
  if (!counted) return;
  if (m.track_slow && ns > m.longest_wait.ns)
    m.longest_wait = {ns, object_key, kind, object_type};
  m.wait_ns[kind] += ns;
  ++m.wait_n[kind];
  if (kind == kWaitObj || kind == kWaitCs) NoteObject(m, object_key, object_type, ns);
}

void SwapBegin() {
  if (!OnMain()) {
    if (Enabled() && !g_warned_not_main) {
      g_warned_not_main = true;
      std::fprintf(stderr, "[frame_chain] a Swap on a thread other than the guest main thread (not measured)\n");
      std::fflush(stderr);
    }
    return;
  }
  MainState& m = g_main;
  ++m.depth;
  m.swap_start = NowNs();
}

void NoteSwapAhead(uint64_t ns) {
  if (!OnMain()) return;
  g_main.ahead_ns += ns;
}

uint64_t LastSwapNs() { return g_last_swap_ns.load(std::memory_order_relaxed); }
uint64_t SwapCount() { return g_swap_count.load(std::memory_order_relaxed); }

void SwapEnd() {
  {
    const uint64_t stamp = NowNs();
    g_swap_count.store(g_swap_count.load(std::memory_order_relaxed) + 1, std::memory_order_relaxed);
    g_last_swap_ns.store(stamp, std::memory_order_relaxed);
  }
  if (!OnMain()) return;
  MainState& m = g_main;
  const uint64_t now = NowNs();
  if (m.depth) --m.depth;
  if (!m.window_start) {
    // The first Swap: the frames start here.
    ResetWindow(m, now);
    if (m.track_slow)
      g_texture_levels.exchange(0, std::memory_order_relaxed);
    m.last_end = now;
    return;
  }
  const uint64_t swap = now - m.swap_start;
  m.swap_ns += swap;
  const uint64_t frame = now - m.last_end;
  m.last_end = now;
  ++m.frames;
  m.frame_ns += frame;
  m.frame_max_ns = std::max(m.frame_max_ns, frame);
  if (m.samples_n < kMaxFrameSamples) m.samples_us[m.samples_n++] = uint32_t(std::min<uint64_t>(frame / 1000, UINT32_MAX));
  if (m.track_slow)
    NoteSlowFrame(m, frame, swap);
  if (now - m.window_start >= kReportNs) {
    Report(m, now);
    // The report's own time goes to the next frame.
    ResetWindow(m, now);
  }
}

void NoteTextureLevel(uint64_t bytes, uint64_t ns) {
  auto previous = g_texture_levels.load(std::memory_order_relaxed);
  for (;;) {
    auto levels = TextureLevels::Unpack(previous);
    levels.Add(bytes, ns);
    if (g_texture_levels.compare_exchange_weak(previous, levels.Pack(), std::memory_order_relaxed))
      return;
  }
}

void AddRender(Render kind, uint64_t ns) {
  g_render_ns[kind].fetch_add(ns, std::memory_order_relaxed);
  g_render_n[kind].fetch_add(1, std::memory_order_relaxed);
}

}  // namespace rex::frame_chain
