/**
 * Frame-chain diagnostic (tekken6-nx 2026-11-20, cvar tekken_frame_chain, default off, init only).
 *
 * Which link of the frame's dependency chain limits the frame rate. One frame = one D3DDevice_Swap_hook call on the
 * guest main thread ("Main XThread"). Per frame:
 *
 *   main thread   time inside the Swap hook (and, of it, the wait for the render thread to fall back to
 *                 kMaxFramesAhead), time blocked in each kind of kernel call (Wait below, measured in the xboxkrnl
 *                 entries around the blocking call), and the rest ("busy")
 *   top objects   the main thread's single-object waits (and critical sections), by object: the three with the most
 *                 wait time
 *   render thread time running work, waiting for work from the game, in the swapchain (acquire + present), on GPU
 *                 fences, in other waits, and blocked on the renderer's state mutex
 *
 * One report every 10 s to stderr (rex_stderr.log on the console), two lines starting "[frame_chain]".
 *
 * Cost when the cvar is off: one thread-local load per kernel wait / signal entry (the main-thread flag, never set),
 * one cached bool per render-thread batch. When on: steady-clock reads only around the measured calls; no allocation
 * on the hot path; the main thread's accumulators are its own (it also writes the report), the render thread's are
 * relaxed atomics.
 */
#pragma once

#include <chrono>
#include <cstdint>

namespace rex::frame_chain {

// The main thread's blocking kernel calls, by kind.
enum Wait : uint8_t {
  kWaitObj,      // KeWaitForSingleObject, NtWaitForSingleObjectEx
  kWaitMulti,    // KeWaitForMultipleObjects, NtWaitForMultipleObjectsEx
  kWaitSigWait,  // NtSignalAndWaitForSingleObjectEx
  kWaitPulse,    // KePulseEvent, NtPulseEvent
  kWaitSet,      // KeSetEvent / NtSetEvent (and the critical section leave's KeSetEvent)
  kWaitSem,      // KeReleaseSemaphore, NtReleaseSemaphore
  kWaitCs,       // RtlEnterCriticalSection's full wait (after the spin)
  kWaitSleep,    // KeDelayExecutionThread, NtYieldExecution
  kWaitRender,   // the renderer's own waits for its render thread (Drain, WaitForFenceProgress) outside the Swap
  kWaitKinds
};

// The render thread's time, by kind.
enum Render : uint8_t {
  kRenderRun,        // running batches of work (the waits below included; busy = run - waits)
  kRenderIdle,       // waiting for work from the game
  kRenderSwapchain,  // swapchain acquire + present
  kRenderFence,      // GPU fence waits
  kRenderOtherWait,  // the other render_thread::WaitScope waits (cleaner, record worker drain)
  kRenderLock,       // blocked taking the renderer's State().mutex for an item (contention with the game's hooks)
  kRenderVertexFills, // queued vertex conversion, fingerprints and fetch copies (subset of busy)
  kRenderKinds
};

// Object type codes for the top objects.
enum ObjType : uint8_t { kObjOther, kObjEvent, kObjSemaphore, kObjMutant, kObjThread, kObjTimer, kObjCritSec };

#if defined(__cpp_constinit)
extern constinit thread_local bool t_main;
#else
extern thread_local bool t_main;
#endif

// The cvar (read once).
bool Enabled();
bool SlowFramesEnabled();
// Called by the guest main thread itself, before it runs guest code: marks it (when the cvar is on).
void MarkMainThread();
#if defined(_WIN32)
// tekken6-nx (2026-11-24): on Windows the runtime is a DLL that exports functions, not thread_local data.
bool OnMain();
#else
inline bool OnMain() { return t_main; }
#endif

inline uint64_t NowNs() {
  return uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(
                      std::chrono::steady_clock::now().time_since_epoch())
                      .count());
}

// Main thread only (OnMain()).
// Returns true when this scope is the outermost measured one (a nested wait is part of its outer one).
bool EnterMain();
// counted: the matching EnterMain returned true (the outermost scope).
void LeaveMain(Wait kind, uint64_t ns, uint32_t object_key, uint8_t object_type, bool counted);
// Swap hook: the begin and the end of the hook (the end closes the frame and may print the report).
void SwapBegin();
void SwapEnd();
// Inside the Swap hook: the wait for the render thread (frames ahead).
void NoteSwapAhead(uint64_t ns);

// tekken6-nx (2026-11-21, tekken_hang_snapshot): every Swap (cvar on or off, any thread) stamps the time of its end
// (NowNs, 0 = no Swap yet) and counts it; the hang watchdog reads both.
uint64_t LastSwapNs();
uint64_t SwapCount();

// Render thread (any thread may call; relaxed atomics).
void AddRender(Render kind, uint64_t ns);
// CPU texture preparation is attributed to the Swap interval where ReadLevel completes.
void NoteTextureLevel(uint64_t bytes, uint64_t ns);

// A measured main-thread kernel call. Construct around the blocking call; Object() names the waited object.
class MainScope {
 public:
  explicit MainScope(Wait kind) : kind_(kind) {
    if (OnMain()) [[unlikely]] {
      if (EnterMain()) {
        start_ = NowNs();
      } else {
        start_ = 1;  // nested: leave without counting
        nested_ = true;
      }
    }
  }
  ~MainScope() {
    if (start_) [[unlikely]]
      LeaveMain(kind_, nested_ ? 0 : NowNs() - start_, key_, type_, !nested_);
  }
  void Object(uint32_t key, uint8_t type) {
    key_ = key;
    type_ = type;
  }
  MainScope(const MainScope&) = delete;
  MainScope& operator=(const MainScope&) = delete;

 private:
  uint64_t start_ = 0;
  uint32_t key_ = 0;
  Wait kind_;
  uint8_t type_ = 0;
  bool nested_ = false;
};

}  // namespace rex::frame_chain
