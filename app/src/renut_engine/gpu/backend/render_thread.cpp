/**
 * @file    gpu/backend/render_thread.cpp
 */
#include "renut_engine/gpu/backend/render_thread.h"
#include "renut_engine/gpu/render/render_census.h"

#include <algorithm>
#include <array>
#include <bit>
#include <system_error>
#include <cstdio>
#include <atomic>
#include <condition_variable>
#include <mutex>
#include <thread>
#include <utility>
#include <fmt/format.h>
#include <string>
#include <rex/frame_latency.h>
#include <rex/frame_chain.h>
#include <vector>

#include "renut_engine/perf/frame_stats.h"
#include "renut_engine/gpu/guest/guest_memory.h"
#include "renut_engine/gpu/log.h"
#include "renut_engine/gpu/backend/video.h"
#include "renut_engine/gpu/banjo_renut_tuning.h"

#include <rex/cvar.h>
#if defined(__SWITCH__)
#include <threading_switch.h>
#endif

// banjo-nx (2026-10-20, from sa2-nx): see g_done_waiters and Run.
REXCVAR_DEFINE_BOOL(banjo_renut_done_notify_waiters, true, "Banjo/Enhancements",
                    "Renut's render thread signals its work done (frames, fences, drains) only when a thread waits for "
                    "it: a signal is a kernel call on the Switch even with nobody waiting, and it made one after every "
                    "batch of work. Off: after every batch, as before")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_BOOL(banjo_renut_prefetch_next, true, "Banjo/Enhancements",
                    "Renut's render thread asks for the next draw's capture (written on the game thread's core) to be "
                    "brought into its cache while it does the draw before it")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_INT32(banjo_renut_wake_batch, 16, "Banjo/Enhancements",
                     "Renut's render thread, when it has caught up and sleeps, is woken by the game's draws only once "
                     "this many are queued (each wake is a kernel call on both threads; the fourth console run woke it "
                     "for almost every draw). Every other item (frames, fences, resources), every wait for it and a "
                     "2 ms timer still wake it at once. 1 or less: every draw wakes it, as before")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_BOOL(banjo_renut_batch_clock, true, "Banjo/Enhancements",
                    "Renut's render thread reads the clock once per batch of work instead of once per item (about "
                    "1,800 items a frame in City Escape); its busy time per frame is the same sum")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif

namespace renut::gpu::render_thread {

namespace {

using Clock = std::chrono::steady_clock;

struct Item {
  std::function<void()> work;
  bool locked = true;  // run with State().mutex held
  const void *prefetch[2] = {};  // banjo-nx (2026-10-20, from sa2-nx, banjo_renut_prefetch_next): fetched while the item before runs
};

// banjo-nx (2026-10-20, from sa2-nx, banjo_renut_prefetch_next): the next item's data into the cache (its first lines).
inline void PrefetchItem(const Item &item) {
  if (const auto *a = static_cast<const char *>(item.prefetch[0])) {
    __builtin_prefetch(a);
    __builtin_prefetch(a + 64);
  }
  if (const auto *b = static_cast<const char *>(item.prefetch[1])) {
    __builtin_prefetch(b);
    __builtin_prefetch(b + 64);
    __builtin_prefetch(b + 128);
    __builtin_prefetch(b + 192);
  }
}

std::mutex g_mutex;
std::condition_variable g_work_cv;  // work queued, or stopping
// banjo-nx (banjo_renut_queue_notify_idle): the render thread is in g_work_cv.wait (under g_mutex). A push notifies only
// then: on the Switch every notify is a kernel call, about 1,500 a frame from the game threads, most of them while the
// render thread is busy and not waiting. The render thread reads the queue under g_mutex before it sleeps, so a push
// it has not seen is never missed.
bool g_render_waiting = false;
std::condition_variable g_done_cv;  // work done: frames, fences, drains; or stopped
// banjo-nx (2026-10-20, from sa2-nx, banjo_renut_done_notify_waiters): threads in g_done_cv's waits (under g_mutex). A done signal goes
// only when there is one: a waiter checks its condition under g_mutex before it counts itself and sleeps, and the
// state it waits for changes under g_mutex too, so no signal it needs is skipped.
int g_done_waiters = 0;
bool DoneNotifyAll() {
  static const bool on = REXCVAR_GET(banjo_renut_done_notify_waiters);
  return !on;
}
// banjo-nx (2026-10-20, from sa2-nx, banjo_renut_wake_batch): the draws queued before a sleeping render thread is woken (1: each).
size_t WakeBatch() {
  static const size_t n = size_t(std::max(1, int(REXCVAR_GET(banjo_renut_wake_batch))));
  return n;
}
std::vector<Item> g_queue;
std::once_flag g_start_once;
std::thread::id g_thread_id;
bool g_running = false;   // the thread takes work
bool g_stopping = false;  // Stop: finish what is queued, then end
uint64_t g_items_queued = 0;
uint64_t g_items_done = 0;
uint64_t g_frames_queued = 0;
uint64_t g_frames_done = 0;
uint64_t g_fences_done = 0;  // fence completions written, plus callbacks reached
uint64_t g_callbacks_reached = 0;
uint64_t g_callbacks_run = 0;
// Fence completions waiting for their callbacks, in order.
struct PendingFence {
  u32 completion_va;
  u32 fence;
  uint64_t after_callback;
};
std::vector<PendingFence> g_pending_fences;

// Requires g_mutex.
void WriteFence(u32 completion_va, u32 fence) {
  if (auto *word = GuestPtr<be_u32>(completion_va))
    *word = fence;
  ++g_fences_done;
}

// Render thread only: time spent running work since the last frame ended, and
// when the running item started; of that time, how much it waited (WaitScope).
Clock::duration g_busy{};
Clock::time_point g_item_start{};
Clock::duration g_waited{};
thread_local bool t_render_thread = false;

constexpr size_t kStageCount = static_cast<size_t>(TraceStage::Count);
constexpr std::array<const char *, kStageCount> kStageNames{
    "between_batches", "batch", "frame", "state_lock", "vertex", "texture", "readback", "recorder",
    "publish", "submit", "fence", "acquire", "present"};
// One render writer. A slot contains the timestamp, stage and sequence together, so the reader can
// reject a slot overwritten during the snapshot without touching the render queue's lock.
constexpr uint64_t kStageHistory = 512;
constexpr uint64_t kStageSerialMask = 4095;
static_assert(kStageCount <= 16 && kStageHistory < kStageSerialMask);
std::array<std::atomic<uint64_t>, kStageHistory> g_stage_history{};
std::atomic<uint64_t> g_stages_written{0};
thread_local TraceStage t_stage = TraceStage::BetweenBatches;

uint64_t ClockUs(Clock::time_point at) {
  return std::chrono::duration_cast<std::chrono::microseconds>(at.time_since_epoch()).count();
}

void PublishStage(TraceStage stage) {
  t_stage = stage;
  const uint64_t seq = g_stages_written.load(std::memory_order_relaxed);
  const uint64_t packed = (ClockUs(Clock::now()) << 16) | ((seq & kStageSerialMask) << 4) |
                          static_cast<uint64_t>(stage);
  g_stage_history[seq % kStageHistory].store(packed, std::memory_order_relaxed);
  g_stages_written.store(seq + 1, std::memory_order_release);
}

void TraceSlowAhead(Clock::time_point begin, Clock::time_point end, uint64_t done) {
  const uint64_t first = ClockUs(begin);
  const uint64_t last = ClockUs(end);
  thread_local uint64_t last_report = 0;
  if (last - first < 25000 || (last_report && last - last_report < 500000))
    return;
  last_report = last;

  std::array<uint64_t, kStageCount> time{};
  uint64_t cursor = last;
  const uint64_t written = g_stages_written.load(std::memory_order_acquire);
  for (uint64_t offset = 0; offset < std::min(written, kStageHistory) && cursor > first; ++offset) {
    const uint64_t seq = written - offset - 1;
    const uint64_t packed = g_stage_history[seq % kStageHistory].load(std::memory_order_relaxed);
    if (((packed >> 4) & kStageSerialMask) != (seq & kStageSerialMask))
      break;
    const uint64_t at = packed >> 16;
    if (at > last)
      continue;
    // Non-increasing timestamps while walking backward. Stop if the snapshot is inconsistent.
    if (at > cursor)
      break;
    const uint64_t from = std::max(first, at);
    time[packed & 15] += cursor - from;
    cursor = from;
  }
  const auto top = std::max_element(time.begin(), time.end());
  const auto top_idx = top - time.begin();
  const uint64_t top_us = *top;
  *top = 0;
  const auto next = std::max_element(time.begin(), time.end());
  std::fprintf(stderr,
               "[pipeline_ahead] swap=%llu render_done=%llu wait=%.2fms top=%s:%.2f next=%s:%.2f history_missing=%.2fms\n",
               static_cast<unsigned long long>(rex::frame_chain::SwapCount()),
               static_cast<unsigned long long>(done), (last - first) / 1000.0,
               top_us ? kStageNames[top_idx] : "-", top_us / 1000.0,
               *next ? kStageNames[next - time.begin()] : "-", *next / 1000.0, (cursor - first) / 1000.0);
  std::fflush(stderr);
}

// tekken6-nx (2026-11-20, tekken_frame_chain): the render thread's report fields (cached once).
bool FrameChainOn() {
  static const bool on = rex::frame_chain::Enabled();
  return on;
}

void Execute(Item &item) {
  if (render_census::FfxiiiEnabled())
    ++render_census::item_serial;
  if (item.locked) {
    // tekken6-nx (tekken_frame_chain): a contended State().mutex is timed (try first: no clock read when free).
    if (FrameChainOn() && t_render_thread) {
      if (!State().mutex.try_lock()) {
        const uint64_t t0 = rex::frame_chain::NowNs();
        {
          TraceScope trace(TraceStage::StateLock);
          State().mutex.lock();
        }
        rex::frame_chain::AddRender(rex::frame_chain::kRenderLock, rex::frame_chain::NowNs() - t0);
      }
      std::lock_guard lock(State().mutex, std::adopt_lock);
      item.work();
      return;
    }
    std::lock_guard lock(State().mutex);
    item.work();
  } else {
    item.work();
  }
}

void Run() {
  {
    // banjo-nx: the thread may be a plain pthread on the Switch (Start), so it records its own id.
    std::lock_guard lock(g_mutex);
    g_thread_id = std::this_thread::get_id();
  }
#if defined(_WIN32)
  SetThreadDescription(GetCurrentThread(), L"renut render");
#endif
  banjo::renut_gpu::OnRenderThreadStart();  // banjo-nx: its core and priority on the Switch
  t_render_thread = true;
  std::vector<Item> batch;
  for (;;) {
    {
      std::unique_lock lock(g_mutex);
      // tekken6-nx (tekken_frame_chain): the wait for work from the game.
      const uint64_t idle_start = FrameChainOn() ? rex::frame_chain::NowNs() : 0;
      g_render_waiting = true;
      if (WakeBatch() > 1) {
        // banjo-nx (from sa2-nx) (banjo_renut_wake_batch): draws queued without a wake are taken within 2 ms at the latest.
        while (g_queue.empty() && !g_stopping)
          g_work_cv.wait_for(lock, std::chrono::milliseconds(2));
      } else {
        g_work_cv.wait(lock, [] { return !g_queue.empty() || g_stopping; });
      }
      g_render_waiting = false;
      if (idle_start)
        rex::frame_chain::AddRender(rex::frame_chain::kRenderIdle, rex::frame_chain::NowNs() - idle_start);
      if (g_queue.empty())
        break;
      batch.swap(g_queue);
    }
    // banjo-nx (2026-10-19, render-thread proposal 19): one clock read per item (the end of one is the start of the
    // next). Its other half, one State().mutex hold across consecutive locked items, hung the emulator run at the
    // first loading screen (a game thread's hook waits on the lock while the render thread's next item waits on it):
    // each item takes the lock on its own, as before.
    TraceScope trace(TraceStage::Batch);
    Clock::time_point start = Clock::now();
    static const bool batch_clock = REXCVAR_GET(banjo_renut_batch_clock);
    if (batch_clock) {
      // banjo-nx (2026-10-20, from sa2-nx): the batch's start for every item (QueueFrame's item counts its frame up to itself from
      // there and leaves the rest of the batch to the next frame), one clock read at the end.
      g_item_start = start;
      static const bool prefetch = REXCVAR_GET(banjo_renut_prefetch_next);
      const size_t n = batch.size();
      for (size_t i = 0; i < n; ++i) {
        if (prefetch && i + 1 < n)
          PrefetchItem(batch[i + 1]);
        Execute(batch[i]);
      }
      const Clock::time_point end = Clock::now();
      g_busy += end - start;
      if (FrameChainOn())  // tekken6-nx (tekken_frame_chain)
        rex::frame_chain::AddRender(
            rex::frame_chain::kRenderRun,
            uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(end - start).count()));
    } else {
      const Clock::time_point batch_start = start;
      for (Item &item : batch) {
        g_item_start = start;
        Execute(item);
        const Clock::time_point end = Clock::now();
        g_busy += end - start;
        start = end;
      }
      if (FrameChainOn())  // tekken6-nx (tekken_frame_chain)
        rex::frame_chain::AddRender(
            rex::frame_chain::kRenderRun,
            uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(start - batch_start).count()));
    }
    bool wake;
    {
      std::lock_guard lock(g_mutex);
      g_items_done += batch.size();
      wake = g_done_waiters > 0 || DoneNotifyAll();
    }
    if (wake)
      g_done_cv.notify_all();
    batch.clear();
  }
  {
    std::lock_guard lock(g_mutex);
    g_running = false;
  }
  g_done_cv.notify_all();
}

// The render thread owns the fill list and its upload allocations throughout
// Run. The worker only reads guest bytes and writes its assigned,
// disjoint staging ranges. No GPU commands or state changes run on the helper.
class FillWorker {
public:
  ~FillWorker() { Stop(); }

  bool Run(const FillBatch &fills, size_t &helper_fills) {
    if (!fills.size || stop_ || !Start())
      return false;
    next_.store(0, std::memory_order_relaxed);
    {
      std::lock_guard lock(mutex_);
      batch_ = fills;
    }
    ready_.notify_one();
    Prepare(fills);
    std::unique_lock lock(mutex_);
    done_.wait(lock, [this] { return batch_.size == 0; });
    helper_fills = completed_;
    return true;
  }

  void Stop() {
    {
      std::lock_guard lock(mutex_);
      stop_ = true;
    }
    if (!worker_.joinable())
      return;
    ready_.notify_one();
    worker_.join();
  }

private:
  bool Start() {
    if (worker_.joinable())
      return true;
    if (tried_)
      return false;
    tried_ = true;
    int core = -1;
#if defined(__SWITCH__)
    const int render_core = RexSwitchCurrentCore();
    if (render_core < 0 || render_core >= 32)
      return false;
    const auto available = RexSwitchAppCoreMask() & ~(1u << render_core);
    if (!available)
      return false;
    core = std::countr_zero(available);
#endif
    try {
      worker_ = std::thread([this, core] { Worker(core); });
    } catch (const std::system_error &e) {
      RGPU_WARN("Render fill helper unavailable: {}", e.what());
      return false;
    }
    return true;
  }

  size_t Prepare(const FillBatch &fills) {
    size_t complete = 0;
    constexpr size_t kGroup = 4;
    for (;;) {
      const size_t first = next_.fetch_add(kGroup, std::memory_order_relaxed);
      if (first >= fills.size)
        return complete;
      const size_t end = std::min(first + kGroup, fills.size);
      for (size_t i = first; i < end; ++i)
        fills.prepare(fills.context, i);
      complete += end - first;
    }
  }

  void Worker(int core) {
#if defined(__SWITCH__)
    if (RexSwitchUnpinCurrentThread(core))
      RexSwitchPinCurrentThreadToCore(core);
    RexSwitchSetCurrentThreadPriorityOk(0x2D);
#else
    (void)core;
#endif
    std::unique_lock lock(mutex_);
    for (;;) {
      ready_.wait(lock, [this] { return stop_ || batch_.size != 0; });
      if (stop_)
        return;
      const auto fills = batch_;
      lock.unlock();
      const size_t complete = Prepare(fills);
      lock.lock();
      completed_ = complete;
      batch_ = {};
      done_.notify_one();
    }
  }

  std::thread worker_;
  std::mutex mutex_;
  std::condition_variable ready_, done_;
  std::atomic<size_t> next_{0};
  FillBatch batch_{};
  size_t completed_ = 0;
  bool tried_ = false, stop_ = false;
};

FillWorker g_fill_worker;

void Start() {
  std::call_once(g_start_once, [] {
    InstallLateInput();  // banjo-nx (2026-10-19, banjo_late_input): the delay function (it reads the key per frame)
    std::lock_guard lock(g_mutex);
    g_running = true;
    // Never joined or destroyed: the renderer's objects are left at exit too
    // (Video::BeginShutdown), and a joinable std::thread destroyed at exit
    // would terminate the process.
    // banjo-nx: a 1 MB stack on the Switch (the conversions and the driver run deep on this thread), never joined
    // (banjo_renut_tuning.h); Run records its id.
    banjo::renut_gpu::StartLongLivedThread(Run, 1024 * 1024);
    RGPU_INFO("Render thread started");
  });
}

// banjo-nx (2026-10-20, from sa2-nx, banjo_renut_wake_batch): before a thread waits for the render thread, the draws queued without a
// wake are handed over. Requires g_mutex.
void WakeForWaitLocked() {
  if (g_render_waiting && !g_queue.empty())
    g_work_cv.notify_one();
}

void Push(std::function<void()> work, bool locked, const void *prefetch0 = nullptr,
          const void *prefetch1 = nullptr, bool draw = false) {
  Start();
  thread_local uint32_t draw_sample = 0;
  const bool sample = draw && render_census::Enabled() && (++draw_sample % 16 == 0);
  const auto start = sample ? render_census::Clock::now() : render_census::Clock::time_point{};
  {
    std::unique_lock lock(g_mutex);
    const uint64_t waited = sample ? render_census::Elapsed(start) : 0;
    // (Work queued by the render thread's own work runs after it.)
    if (g_running && !g_stopping) {
      g_queue.push_back({std::move(work), locked, {prefetch0, prefetch1}});
      ++g_items_queued;
      // banjo-nx (from sa2-nx) (banjo_renut_wake_batch): a draw wakes a sleeping render thread once enough are queued.
      const bool wake = (g_render_waiting && (!draw || g_queue.size() >= WakeBatch())) ||
                        !banjo::renut_gpu::QueueNotifyIdleOn();
      lock.unlock();
      if (wake)
        g_work_cv.notify_one();
      if (sample)
        render_census::NoteQueueLock(waited);
      return;
    }
  }
  // Shutting down: nothing takes work any more, so run it here.
  Item item{std::move(work), locked, {}};
  Execute(item);
}

} // namespace

bool ShareFillBatch(const FillBatch &batch, size_t &helper_fills) {
  return g_fill_worker.Run(batch, helper_fills);
}

void StopFillWorker() {
  g_fill_worker.Stop();
}

void Queue(std::function<void()> work) {
  Push(std::move(work), true);
}

void QueueUnlocked(std::function<void()> work) {
  Push(std::move(work), false);
}

void QueuePrefetched(const void *prefetch0, const void *prefetch1, std::function<void()> work) {
  Push(std::move(work), true, prefetch0, prefetch1, true);  // banjo-nx (from sa2-nx): the game's draws (banjo_renut_wake_batch)
}

void QueueFenceCompletion(u32 completion_va, u32 fence, uint64_t after_callback) {
  QueueUnlocked([completion_va, fence, after_callback] {
    bool wake;
    {
      std::lock_guard lock(g_mutex);
      // Fences complete in order: behind one still waiting, this one waits too.
      if (g_pending_fences.empty() && after_callback <= g_callbacks_run)
        WriteFence(completion_va, fence);
      else
        g_pending_fences.push_back({completion_va, fence, after_callback});
      wake = g_done_waiters > 0 || DoneNotifyAll();  // banjo-nx (from sa2-nx)
    }
    if (wake)
      g_done_cv.notify_all();
  });
}

void QueueCallbackMarker(uint64_t seq) {
  QueueUnlocked([seq] {
    bool wake;
    {
      std::lock_guard lock(g_mutex);
      g_callbacks_reached = std::max(g_callbacks_reached, seq);
      ++g_fences_done;  // wakes WaitForFenceProgress: there is a callback to run
      wake = g_done_waiters > 0 || DoneNotifyAll();  // banjo-nx (from sa2-nx)
    }
    if (wake)
      g_done_cv.notify_all();
  });
}

uint64_t CallbacksReached() {
  std::lock_guard lock(g_mutex);
  return g_callbacks_reached;
}

void CallbacksRun(uint64_t seq) {
  bool wake;
  {
    std::lock_guard lock(g_mutex);
    g_callbacks_run = std::max(g_callbacks_run, seq);
    size_t written = 0;
    for (; written < g_pending_fences.size(); ++written) {
      const PendingFence &p = g_pending_fences[written];
      if (p.after_callback > g_callbacks_run)
        break;
      WriteFence(p.completion_va, p.fence);
    }
    g_pending_fences.erase(g_pending_fences.begin(), g_pending_fences.begin() + written);
    wake = g_done_waiters > 0 || DoneNotifyAll();  // banjo-nx (from sa2-nx)
  }
  if (wake)
    g_done_cv.notify_all();
}

bool WaitForFenceProgress(std::chrono::milliseconds timeout) {
  std::unique_lock lock(g_mutex);
  if (!g_running || g_stopping)
    return false;
  const uint64_t seen = g_fences_done;
  // tekken6-nx (tekken_frame_chain): the game's main thread waiting for the render thread (outside the Swap).
  rex::frame_chain::MainScope chain(rex::frame_chain::kWaitRender);
  WakeForWaitLocked();  // banjo-nx (from sa2-nx) (banjo_renut_wake_batch)
  ++g_done_waiters;  // banjo-nx (from sa2-nx) (banjo_renut_done_notify_waiters)
  g_done_cv.wait_for(lock, timeout,
                     [seen] { return g_fences_done != seen || !g_running || g_stopping; });
  --g_done_waiters;
  return g_running && !g_stopping;
}

// banjo-nx (2026-10-19, input latency, banjo_late_input): the game reads its controller at the start of a frame, and
// when the render thread is the slower of the two its frame then waits: the Swap below waits for the frame before to be
// done. The slack of a frame is that wait plus the delay its read already had: how much later the read could have been
// without the frame reaching the render thread any later. The next frame's first read is delayed by the least slack of
// the last kSlackFrames frames less kLateInputMarginUs (rex::latency::SetFrameReadDelayFunction): a frame whose game
// work runs long eats into the margin, and a run of them shrinks the delay; with the game thread or the frame cap the
// limit there is no wait, and the delay goes to 0.
namespace {
constexpr uint32_t kSlackFrames = 16;
constexpr uint64_t kLateInputMarginUs = 2000;
// banjo-nx (2026-10-19, latency batch 3): the most the read is delayed: 80 % of the game's frame interval (averaged over
// the frames at QueueFrame), at most kLateInputCeilingUs. A fixed 30 ms held it back where the GPU is the limit: the
// console's 18 fps scenes (package 35) delayed 30.0 ms with more slack than that.
constexpr uint64_t kLateInputCeilingUs = 60000;
std::atomic<uint64_t> g_frame_interval_us{0};
uint64_t g_last_queue_us = 0;
uint64_t g_slack_us[kSlackFrames] = {};
uint32_t g_slack_next = 0;
uint32_t g_slack_count = 0;
std::atomic<uint32_t> g_applied_delay_us{0};
std::atomic<uint64_t> g_delay_total_us{0}, g_delay_frames{0};

// banjo-nx (2026-10-20, package 36s): banjo_late_input_gpu (36l-36r), a second slack from the GPU queue's wait, is gone:
// on the console it gained nothing at the 30 fps cap and, in scenes just able to hold 30, its delay ratcheted up to
// 29 ms and cost about 6 fps (36q). docs/deko3d-renderer.md section 25.

uint32_t LateInputDelay() {
  if (!banjo::renut_gpu::LateInputOn() || g_slack_count < kSlackFrames) {
    g_applied_delay_us.store(0, std::memory_order_relaxed);
    return 0;
  }
  uint64_t least = UINT64_MAX;
  for (uint64_t s : g_slack_us)
    least = std::min(least, s);
  const uint64_t interval = g_frame_interval_us.load(std::memory_order_relaxed);
  const uint64_t ceiling = std::min(kLateInputCeilingUs, interval * 4 / 5);
  const uint64_t delay = least > kLateInputMarginUs ? std::min(least - kLateInputMarginUs, ceiling) : 0;
  g_applied_delay_us.store(uint32_t(delay), std::memory_order_relaxed);
  g_delay_total_us.fetch_add(delay, std::memory_order_relaxed);
  g_delay_frames.fetch_add(1, std::memory_order_relaxed);
  return uint32_t(delay);
}
}  // namespace

void InstallLateInput() {
  rex::latency::SetFrameReadDelayFunction(&LateInputDelay);
}

std::string LateInputReport() {
  const uint64_t frames = g_delay_frames.exchange(0, std::memory_order_relaxed);
  const uint64_t total = g_delay_total_us.exchange(0, std::memory_order_relaxed);
  return frames ? fmt::format("late input: the first controller read of a frame delayed {:.1f} ms on average ({} "
                              "frames)",
                              double(total) / 1000.0 / double(frames), frames)
                : std::string("late input: no delay");
}

uint64_t QueueFrame(std::function<void()> work) {
  {
    std::lock_guard lock(g_mutex);
    ++g_frames_queued;
  }
  QueueUnlocked([work = std::move(work)] {
    {
      TraceScope trace(TraceStage::Frame);
      work();
    }
    // The frame's work includes this item's (the present); Run adds the whole
    // item afterwards, so what is left of it counts to the next frame.
    const Clock::duration so_far = Clock::now() - g_item_start;
    const double busy_ms =
        std::chrono::duration<double, std::milli>(g_busy + so_far - g_waited).count();
    g_busy = -so_far;
    g_waited = {};
    frame_stats::OnRenderThreadFrame(busy_ms);
    bool wake;
    {
      std::lock_guard lock(g_mutex);
      ++g_frames_done;
      wake = g_done_waiters > 0 || DoneNotifyAll();  // banjo-nx (from sa2-nx)
    }
    if (wake)
      g_done_cv.notify_all();
    if (t_render_thread) {
      if (render_census::Enabled())
        render_census::EndFrame();
      if (render_census::FfxiiiEnabled())
        render_census::EndFfxiiiFrame();
      if (render_census::PipelineEnabled())
        render_census::EndPipelineFrame();
    }
  });
  const auto waiting = std::chrono::steady_clock::now();
  std::unique_lock lock(g_mutex);
  WakeForWaitLocked();  // banjo-nx (from sa2-nx) (banjo_renut_wake_batch)
  ++g_done_waiters;  // banjo-nx (from sa2-nx) (banjo_renut_done_notify_waiters)
  g_done_cv.wait(lock, [] {
    return !g_running || g_stopping || g_frames_queued - g_frames_done <= kMaxFramesAhead;
  });
  --g_done_waiters;
  const uint64_t done = g_frames_done;
  lock.unlock();
  const auto wait_end = std::chrono::steady_clock::now();
  const auto waited = wait_end - waiting;
  if (render_census::PipelineEnabled())
    TraceSlowAhead(waiting, wait_end, done);
  frame_stats::OnGameThreadWait(std::chrono::duration<double, std::milli>(waited).count());
  // tekken6-nx (tekken_frame_chain): the Swap's wait for the render thread (frames ahead).
  rex::frame_chain::NoteSwapAhead(uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(waited).count()));
  // banjo-nx (2026-10-19, banjo_late_input): this frame's slack (the game thread only: one Swap at a time).
  {
    // The game's frame interval (the game thread only), averaged over about 16 frames.
    const uint64_t at = uint64_t(
        std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now().time_since_epoch())
            .count());
    if (g_last_queue_us && at > g_last_queue_us) {
      const uint64_t gap = std::min<uint64_t>(at - g_last_queue_us, 1'000'000);
      const uint64_t avg = g_frame_interval_us.load(std::memory_order_relaxed);
      g_frame_interval_us.store(avg ? avg + (int64_t(gap) - int64_t(avg)) / 16 : gap, std::memory_order_relaxed);
    }
    g_last_queue_us = at;
  }
  g_slack_us[g_slack_next] =
      uint64_t(std::chrono::duration_cast<std::chrono::microseconds>(waited).count()) +
      g_applied_delay_us.exchange(0, std::memory_order_relaxed);
  g_slack_next = (g_slack_next + 1) % kSlackFrames;
  g_slack_count = std::min(g_slack_count + 1, kSlackFrames);
  return done;
}

uint64_t FramesDone() {
  std::lock_guard lock(g_mutex);
  return g_frames_done;
}

void Drain() {
  std::unique_lock lock(g_mutex);
  if (!g_running || std::this_thread::get_id() == g_thread_id)
    return;
  const uint64_t target = g_items_queued;
  rex::frame_chain::MainScope chain(rex::frame_chain::kWaitRender);  // tekken6-nx (tekken_frame_chain)
  WakeForWaitLocked();  // banjo-nx (from sa2-nx) (banjo_renut_wake_batch)
  ++g_done_waiters;  // banjo-nx (from sa2-nx) (banjo_renut_done_notify_waiters)
  g_done_cv.wait(lock, [target] { return !g_running || g_items_done >= target; });
  --g_done_waiters;
}

bool IsRenderThread() {
  return t_render_thread;
}

TraceScope::TraceScope(TraceStage stage) {
  if (render_census::PipelineEnabled() && t_render_thread && stage != TraceStage::Count && stage != t_stage) {
    previous_ = t_stage;
    PublishStage(stage);
  }
}

TraceScope::~TraceScope() {
  if (previous_ != TraceStage::Count)
    PublishStage(previous_);
}

WaitScope::WaitScope(WaitKind kind)
    : start_(Clock::now()), counted_(t_render_thread), kind_(kind),
      trace_(kind == WaitKind::kFence ? TraceStage::Fence
             : kind == WaitKind::kSwapchain ? TraceStage::Swapchain : TraceStage::Count) {}

WaitScope::~WaitScope() {
  if (counted_) {
    const Clock::duration d = Clock::now() - start_;
    g_waited += d;
    if (FrameChainOn()) {  // tekken6-nx (tekken_frame_chain)
      const auto kind = kind_ == WaitKind::kSwapchain ? rex::frame_chain::kRenderSwapchain
                        : kind_ == WaitKind::kFence   ? rex::frame_chain::kRenderFence
                                                      : rex::frame_chain::kRenderOtherWait;
      rex::frame_chain::AddRender(kind, uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(d).count()));
    }
  }
}

void Stop() {
  {
    std::lock_guard lock(g_mutex);
    if (!g_running || g_stopping)
      return;
    g_stopping = true;
  }
  g_work_cv.notify_all();
  g_done_cv.notify_all();
}

} // namespace renut::gpu::render_thread
