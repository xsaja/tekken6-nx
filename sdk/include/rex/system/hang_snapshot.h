/**
 * Hang snapshot (tekken6-nx 2026-11-21, cvar tekken_hang_snapshot, default on, init only).
 *
 * The rare 0 fps freeze: every guest thread parked, one thread polling. When the guest main thread has not called
 * D3DDevice_Swap for 4 s (after its first Swap), a watchdog thread prints ONE snapshot to stderr ("[hang]" lines,
 * rex_stderr.log on the console), and again every further 30 s while the freeze lasts:
 *
 *   - per guest thread: its current (or last) kernel wait, the objects, since when it waits on them without success,
 *     the last result, how many of those waits timed out in a row;
 *   - per object in a current wait: its host state and its last 4 signal operations (who, how long ago);
 *   - per critical-section wait: the section's owner, lock count, recursion count;
 *   - the guest main thread's last 8 waits.
 *
 * Format: docs/switch-port-log.md, "Hang snapshot (2026-11-21)". Diagnostic only: no wait or signal changes.
 *
 * Cost (on): per kernel wait, a thread-local lookup, ~20 plain stores, two steady-clock reads and one acquire load;
 * per signal (SetEvent / PulseEvent / ResetEvent / ReleaseSemaphore / ReleaseMutant / the signal half of
 * SignalAndWait), one out-of-line call: a thread-local lookup, a clock read and three relaxed stores. Off: one cached
 * bool per wait and per signal.
 */
#pragma once

#include <cstdint>

namespace rex::system {
class KernelState;
class XObject;
}  // namespace rex::system

namespace rex::system::hang {

enum Kind : uint8_t {
  kKindNone,
  kKindObj,      // KeWaitForSingleObject / NtWaitForSingleObjectEx
  kKindSigWait,  // NtSignalAndWaitForSingleObjectEx (objects: [0] waited on, [1] signalled)
  kKindAny,      // Wait*MultipleObjects, wait any
  kKindAll,      // Wait*MultipleObjects, wait all
  kKindCs,       // RtlEnterCriticalSection's full wait (object: the section's own event)
  kKindSleep,    // KeDelayExecutionThread (key: the interval in ms)
  kKindYield,    // NtYieldExecution
};

// Signal operations recorded on the object (XObject::NoteSignalOp).
enum Op : uint8_t {
  kOpSet = 1,     // SetEvent (KeSetEvent / NtSetEvent / host Set)
  kOpPulse,       // PulseEvent
  kOpReset,       // ResetEvent / ClearEvent
  kOpRelease,     // ReleaseSemaphore (arg: count, previous count)
  kOpMutant,      // ReleaseMutant
  kOpSigWait,     // the signal half of SignalAndWait
};

// The cvar (read once).
bool Enabled();

// The waits, called by XObject::Wait / SignalAndWait / WaitMultiple (Scope below) and the kernel sleep entries.
// Only the outermost wait of a thread is recorded (a wait inside a user callback of an alertable wait is not).
// timeout_ms: ~0u = none.
void WaitBegin(Kind kind, uint32_t count, XObject* const* objects, uint32_t timeout_ms, bool alertable);
void WaitBeginKey(Kind kind, uint32_t key, uint32_t timeout_ms, bool alertable);  // a sleep (no object)
void WaitDone(uint32_t status);

// RtlEnterCriticalSection: the next object wait of this thread is that section's (kKindCs).
void NoteCriticalSection(uint32_t cs_guest);

// The thread leaves (XThread::Exit, or Execute returned): its record is released.
void ThreadExit();

// Starts the watchdog thread once (the guest main thread calls it as it starts).
void StartWatchdog(KernelState* kernel_state);

// A wait scope: WaitBegin in the constructor, WaitDone(status) in the destructor (set_status before it ends).
class Scope {
 public:
  Scope(Kind kind, uint32_t count, XObject* const* objects, uint32_t timeout_ms, bool alertable) {
    if (Enabled()) {
      on_ = true;
      WaitBegin(kind, count, objects, timeout_ms, alertable);
    }
  }
  ~Scope() {
    if (on_) WaitDone(status_);
  }
  void set_status(uint32_t status) { status_ = status; }
  Scope(const Scope&) = delete;
  Scope& operator=(const Scope&) = delete;

 private:
  uint32_t status_ = 0xFFFFFFFFu;  // unknown (left by an exit path that did not set it)
  bool on_ = false;
};

}  // namespace rex::system::hang
