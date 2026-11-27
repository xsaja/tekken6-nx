// Hang snapshot (rex/system/hang_snapshot.h). tekken6-nx 2026-11-21.
//
// Per guest thread, a record of its current / last kernel wait (a fixed pool of records, one per thread, registered
// on its first wait or signal); per object, its last 4 signal operations (XObject::NoteSignalOp). A watchdog thread
// prints both when the guest main thread stops calling D3DDevice_Swap.
//
// Object lifetime: a record's object pointers are used only while the record says "in wait" (in_wait), and only
// while the watchdog holds g_busy. A thread clears in_wait after its wait returns and, if g_busy is set, waits for
// the watchdog to finish before it returns to its caller, which holds the references to those objects (Dekker with
// seq_cst on both sides: either the watchdog sees in_wait false, or the thread sees g_busy and waits).
#include <rex/system/hang_snapshot.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

#include <rex/cvar.h>
#include <rex/frame_chain.h>
#include <rex/types.h>
#include <rex/system/xmemory.h>
#include <rex/system/kernel_state.h>
#include <rex/system/xmutant.h>
#include <rex/system/xobject.h>
#include <rex/system/xthread.h>
#include <rex/system/xtypes.h>

REXCVAR_DEFINE_BOOL(tekken_hang_snapshot, true, "Tekken",
                    "Hang snapshot: when the guest main thread has not called D3DDevice_Swap for 4 s (after its first "
                    "Swap), print once to stderr ('[hang]' lines) every guest thread's current kernel wait, the "
                    "waited objects' state and last 4 signal operations, and critical-section owners; again every "
                    "30 s while it lasts (docs/switch-port-log.md). Costs a few stores per wait and per signal")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);

namespace rex::system {

namespace hang {
namespace {

constexpr uint32_t kMaxRecords = 96;
constexpr uint32_t kMaxObjs = 4;
constexpr uint32_t kHistory = 8;
constexpr uint64_t kFirstNs = 4'000'000'000ull;    // no Swap this long: the first snapshot
constexpr uint64_t kRepeatNs = 30'000'000'000ull;  // then one every this long while it lasts
constexpr uint32_t kMaxLines = 200;

inline uint64_t NowNs() { return rex::frame_chain::NowNs(); }

struct HistEntry {
  uint64_t end_ns;
  uint32_t key;
  uint32_t status;
  uint32_t dur_us;
  uint8_t kind;
  char type;
};

struct Record {
  std::atomic<uint32_t> used{0};  // 0 free, 1 being set up, 2 in use
  // The thread.
  uint32_t tid = 0;
  uint32_t kthread = 0;  // the guest KTHREAD (critical sections name their owner by it)
  uint32_t handle = 0;
  int32_t priority = 0;
  bool main = false;
  char name[48] = {};
  // The current (in_wait) or last wait.
  uint8_t kind = kKindNone;
  uint8_t nobj = 0;
  bool alertable = false;
  bool last_ok = false;     // the last wait was satisfied (a new streak starts with the next one)
  uint32_t total = 0;       // objects of a multiple wait (nobj: the ones kept)
  uint32_t timeout_ms = 0;  // ~0u: none
  uint32_t keys[kMaxObjs] = {};   // handles (or the sleep's interval)
  uint32_t addrs[kMaxObjs] = {};  // the objects' guest addresses (0: none)
  char types[kMaxObjs] = {};
  XObject* objs[kMaxObjs] = {};  // valid only while in_wait and g_busy (see the top of the file)
  uint32_t cs = 0;               // the section of a kKindCs wait
  uint32_t pending_cs = 0;       // NoteCriticalSection, for the next WaitBegin
  std::atomic<bool> in_wait{false};
  uint32_t depth = 0;  // nested waits (only the outermost is recorded)
  uint64_t wait_start_ns = 0;
  uint64_t streak_start_ns = 0;  // first wait on these objects since the last satisfied one
  uint64_t last_done_ns = 0;
  uint32_t last_status = 0xFFFFFFFFu;
  uint32_t streak_waits = 0;
  uint32_t streak_timeouts = 0;
  uint32_t streak_apcs = 0;
  uint64_t seq = 0;  // waits begun
  HistEntry hist[kHistory] = {};
  uint32_t hist_n = 0;
};

Record g_records[kMaxRecords];
thread_local Record* t_rec = nullptr;
std::atomic<bool> g_busy{false};
std::atomic<bool> g_watchdog_started{false};

char TypeLetter(XObject* object) {
  switch (object->type()) {
    case XObject::Type::Event: return 'E';
    case XObject::Type::Semaphore: return 'S';
    case XObject::Type::Mutant: return 'M';
    case XObject::Type::Thread: return 'T';
    case XObject::Type::Timer: return 'R';
    default: return 'O';
  }
}

Record* GetRecord() {
  if (Record* r = t_rec) return r;
  if (!XThread::IsInThread()) return nullptr;  // a host thread: not recorded
  XThread* thread = XThread::GetCurrentThread();
  for (Record& r : g_records) {
    uint32_t expected = 0;
    if (!r.used.compare_exchange_strong(expected, 1, std::memory_order_acq_rel)) continue;
    r.tid = thread->thread_id();
    r.kthread = thread->guest_object();
    r.handle = thread->handle();
    r.priority = thread->priority();
    r.main = thread->main_thread() && thread->is_guest_thread();
    std::snprintf(r.name, sizeof(r.name), "%s", thread->thread_name().c_str());
    r.kind = kKindNone;
    r.nobj = 0;
    r.total = 0;
    r.last_ok = false;
    r.pending_cs = 0;
    r.cs = 0;
    r.depth = 0;
    r.in_wait.store(false, std::memory_order_relaxed);
    r.wait_start_ns = r.streak_start_ns = r.last_done_ns = 0;
    r.last_status = 0xFFFFFFFFu;
    r.streak_waits = r.streak_timeouts = r.streak_apcs = 0;
    r.seq = 0;
    r.hist_n = 0;
    r.used.store(2, std::memory_order_release);
    t_rec = &r;
    return &r;
  }
  return nullptr;  // pool full
}

// The thread's current guest thread id for a signal record (0xFFFFFFFF: a host thread).
uint32_t CurrentTid() {
  if (Record* r = t_rec) return r->tid;
  if (Record* r = GetRecord()) return r->tid;
  return 0xFFFFFFFFu;
}

void Begin(Record* r, Kind kind, uint32_t n, uint32_t total, XObject* const* objects, const uint32_t* keys,
           const uint32_t* addrs, const char* types, uint32_t timeout_ms, bool alertable) {
  bool same = r->kind == kind && r->nobj == n && r->total == total;
  for (uint32_t i = 0; i < n && same; ++i) same = r->keys[i] == keys[i];
  const uint64_t now = NowNs();
  if (!same || r->last_ok) {
    r->streak_start_ns = now;
    r->streak_waits = r->streak_timeouts = r->streak_apcs = 0;
  }
  r->kind = kind;
  r->nobj = uint8_t(n);
  r->total = total;
  r->alertable = alertable;
  r->timeout_ms = timeout_ms;
  for (uint32_t i = 0; i < n; ++i) {
    r->keys[i] = keys[i];
    r->addrs[i] = addrs[i];
    r->types[i] = types[i];
    r->objs[i] = objects ? objects[i] : nullptr;
  }
  r->wait_start_ns = now;
  ++r->streak_waits;
  ++r->seq;
  r->in_wait.store(true, std::memory_order_seq_cst);
}

}  // namespace

bool Enabled() {
  static const bool on = REXCVAR_GET(tekken_hang_snapshot);
  return on;
}

void WaitBegin(Kind kind, uint32_t count, XObject* const* objects, uint32_t timeout_ms, bool alertable) {
  Record* r = GetRecord();
  if (!r) return;
  if (r->depth++) return;  // nested (a wait inside a user callback): only the outermost is recorded
  Kind k = kind;
  r->cs = 0;
  if (r->pending_cs) {
    if (kind == kKindObj) {
      k = kKindCs;
      r->cs = r->pending_cs;
    }
    r->pending_cs = 0;
  }
  const uint32_t n = std::min(count, kMaxObjs);
  uint32_t keys[kMaxObjs], addrs[kMaxObjs];
  char types[kMaxObjs];
  for (uint32_t i = 0; i < n; ++i) {
    keys[i] = objects[i]->handle();
    addrs[i] = objects[i]->guest_object();
    types[i] = TypeLetter(objects[i]);
  }
  Begin(r, k, n, count, objects, keys, addrs, types, timeout_ms, alertable);
}

void WaitBeginKey(Kind kind, uint32_t key, uint32_t timeout_ms, bool alertable) {
  Record* r = GetRecord();
  if (!r) return;
  if (r->depth++) return;
  r->cs = 0;
  const uint32_t addr = 0;
  const char type = '-';
  Begin(r, kind, 1, 0, nullptr, &key, &addr, &type, timeout_ms, alertable);
}

void WaitDone(uint32_t status) {
  Record* r = t_rec;
  if (!r || r->depth == 0) return;
  if (--r->depth) return;
  r->in_wait.store(false, std::memory_order_seq_cst);
  if (g_busy.load(std::memory_order_seq_cst)) [[unlikely]] {
    // The watchdog may be reading this wait's objects: wait until it is done (only during a snapshot).
    while (g_busy.load(std::memory_order_acquire)) std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  const uint64_t now = NowNs();
  const bool object_wait = r->kind != kKindSleep && r->kind != kKindYield;
  r->last_status = status;
  r->last_done_ns = now;
  r->last_ok = object_wait && status < 0x40;  // X_STATUS_SUCCESS / WAIT_n
  if (status == X_STATUS_TIMEOUT) ++r->streak_timeouts;
  if (status == X_STATUS_USER_APC) ++r->streak_apcs;
  HistEntry& h = r->hist[r->hist_n++ % kHistory];
  h.end_ns = now;
  h.key = r->kind == kKindCs ? r->cs : r->keys[0];
  h.status = status;
  h.dur_us = uint32_t(std::min<uint64_t>((now - r->wait_start_ns) / 1000, 0xFFFFFFFFu));
  h.kind = r->kind;
  h.type = r->kind == kKindCs ? 'C' : (r->nobj ? r->types[0] : '-');
}

void NoteCriticalSection(uint32_t cs_guest) {
  if (Record* r = GetRecord()) r->pending_cs = cs_guest;
}

void ThreadExit() {
  Record* r = t_rec;
  if (!r) return;
  r->in_wait.store(false, std::memory_order_seq_cst);
  if (g_busy.load(std::memory_order_seq_cst)) [[unlikely]] {
    while (g_busy.load(std::memory_order_acquire)) std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  r->depth = 0;
  t_rec = nullptr;
  r->used.store(0, std::memory_order_release);
}

namespace {

const char* KindName(uint8_t kind) {
  switch (kind) {
    case kKindObj: return "obj";
    case kKindSigWait: return "sigwait";
    case kKindAny: return "any";
    case kKindAll: return "all";
    case kKindCs: return "cs";
    case kKindSleep: return "sleep";
    case kKindYield: return "yield";
    default: return "none";
  }
}

const char* OpName(uint8_t op) {
  switch (op) {
    case kOpSet: return "Set";
    case kOpPulse: return "Pulse";
    case kOpReset: return "Reset";
    case kOpRelease: return "Release";
    case kOpMutant: return "RelMutant";
    case kOpSigWait: return "SigWait";
    default: return "?";
  }
}

void StatusName(uint32_t status, char* out, size_t size) {
  if (status == 0) {
    std::snprintf(out, size, "ok");
  } else if (status < 0x40) {
    std::snprintf(out, size, "ok#%u", status);
  } else if (status == X_STATUS_TIMEOUT) {
    std::snprintf(out, size, "timeout");
  } else if (status == X_STATUS_USER_APC) {
    std::snprintf(out, size, "apc");
  } else if (status >= 0x80 && status < 0xC0) {
    std::snprintf(out, size, "abandoned");
  } else if (status == 0xFFFFFFFFu) {
    std::snprintf(out, size, "-");
  } else {
    std::snprintf(out, size, "%08X", status);
  }
}

// What the watchdog copies out of a record (and of its objects while in_wait, under g_busy).
struct ThreadCopy {
  Record* rec;
  uint32_t tid, kthread, handle;
  int32_t priority;
  bool main;
  char name[48];
  uint8_t kind, nobj;
  bool alertable, in_wait, objs_seen;
  uint32_t total, timeout_ms, cs;
  uint32_t keys[kMaxObjs], addrs[kMaxObjs];
  char types[kMaxObjs];
  int obj_index[kMaxObjs];  // into the object table (-1: not captured)
  uint64_t wait_start_ns, streak_start_ns, last_done_ns, seq;
  uint32_t last_status, streak_waits, streak_timeouts, streak_apcs;
  HistEntry hist[kHistory];
  uint32_t hist_n;
  // A critical section's fields (kKindCs, while in wait).
  bool cs_read;
  int32_t cs_lock_count, cs_recursion;
  uint32_t cs_owner;
};

struct ObjectCopy {
  XObject* ptr;  // identity only (not dereferenced after the copy)
  char type;
  uint32_t handle, addr;
  std::string name, state;
  uint32_t mutant_owner;
  uint32_t op_n;
  uint64_t op_when[XObject::kSignalOps];
  uint64_t op_what[XObject::kSignalOps];
};

void CopyRecord(Record& r, ThreadCopy& t) {
  t.rec = &r;
  t.tid = r.tid;
  t.kthread = r.kthread;
  t.handle = r.handle;
  t.priority = r.priority;
  t.main = r.main;
  std::memcpy(t.name, r.name, sizeof(t.name));
  t.name[sizeof(t.name) - 1] = 0;
  t.kind = r.kind;
  t.nobj = std::min<uint8_t>(r.nobj, kMaxObjs);
  t.alertable = r.alertable;
  t.total = r.total;
  t.timeout_ms = r.timeout_ms;
  t.cs = r.cs;
  for (uint32_t i = 0; i < kMaxObjs; ++i) {
    t.keys[i] = r.keys[i];
    t.addrs[i] = r.addrs[i];
    t.types[i] = r.types[i];
    t.obj_index[i] = -1;
  }
  t.wait_start_ns = r.wait_start_ns;
  t.streak_start_ns = r.streak_start_ns;
  t.last_done_ns = r.last_done_ns;
  t.seq = r.seq;
  t.last_status = r.last_status;
  t.streak_waits = r.streak_waits;
  t.streak_timeouts = r.streak_timeouts;
  t.streak_apcs = r.streak_apcs;
  std::memcpy(t.hist, r.hist, sizeof(t.hist));
  t.hist_n = r.hist_n;
}

int CaptureObject(std::vector<ObjectCopy>& objects, XObject* object) {
  for (size_t i = 0; i < objects.size(); ++i) {
    if (objects[i].ptr == object) return int(i);
  }
  if (objects.size() >= 64) return -1;
  ObjectCopy o;
  o.ptr = object;
  o.type = TypeLetter(object);
  o.handle = object->handle();
  o.addr = object->guest_object();
  o.name = object->name();
  o.state = object->DebugWaitState();
  o.mutant_owner = object->type() == XObject::Type::Mutant ? static_cast<XMutant*>(object)->owner_tid() : 0;
  o.op_n = object->signal_op_count();
  for (uint32_t i = 0; i < XObject::kSignalOps; ++i) {
    o.op_when[i] = object->signal_op(i).when_ns.load(std::memory_order_relaxed);
    o.op_what[i] = object->signal_op(i).what.load(std::memory_order_relaxed);
  }
  objects.push_back(std::move(o));
  return int(objects.size() - 1);
}

// Under g_busy, the record in wait: its objects (and its critical section) are copied.
void CaptureWait(KernelState* ks, Record& r, ThreadCopy& t, std::vector<ObjectCopy>& objects) {
  CopyRecord(r, t);
  t.in_wait = true;
  t.objs_seen = true;
  for (uint32_t i = 0; i < t.nobj; ++i) {
    if (XObject* object = r.objs[i]) t.obj_index[i] = CaptureObject(objects, object);
  }
  t.cs_read = false;
  if (t.kind == kKindCs && t.cs && ks) {
    // X_RTL_CRITICAL_SECTION (xboxkrnl_rtl.cpp): +0x10 lock_count (as that code stores it), +0x14 recursion_count
    // (big-endian), +0x18 owning_thread (big-endian KTHREAD).
    const uint8_t* p = ks->memory()->TranslateVirtual<const uint8_t*>(t.cs);
    int32_t lock_count;
    std::memcpy(&lock_count, p + 0x10, 4);
    uint32_t recursion, owner;
    std::memcpy(&recursion, p + 0x14, 4);
    std::memcpy(&owner, p + 0x18, 4);
    t.cs_lock_count = lock_count;
    t.cs_recursion = int32_t(rex::byte_swap(recursion));
    t.cs_owner = rex::byte_swap(owner);
    t.cs_read = true;
  }
}

struct Printer {
  uint32_t lines = 0;
  bool truncated = false;
  bool Line(const char* text) {
    if (lines >= kMaxLines) {
      truncated = true;
      return false;
    }
    ++lines;
    std::fprintf(stderr, "[hang] %s\n", text);
    return true;
  }
};

uint32_t TidOfKthread(const std::vector<ThreadCopy>& threads, uint32_t kthread) {
  for (const ThreadCopy& t : threads) {
    if (t.kthread == kthread) return t.tid;
  }
  return 0;
}

void Snapshot(KernelState* ks, unsigned number, uint64_t stalled_ns) {
  std::vector<ThreadCopy> threads;
  std::vector<ObjectCopy> objects;
  threads.reserve(kMaxRecords);
  objects.reserve(64);

  // Pass 1: every record.
  g_busy.store(true, std::memory_order_seq_cst);
  for (Record& r : g_records) {
    if (r.used.load(std::memory_order_acquire) != 2) continue;
    ThreadCopy t{};
    if (r.in_wait.load(std::memory_order_seq_cst)) {
      CaptureWait(ks, r, t, objects);
    } else {
      CopyRecord(r, t);
      t.in_wait = false;
      t.objs_seen = false;
    }
    threads.push_back(t);
  }
  g_busy.store(false, std::memory_order_seq_cst);

  // Pass 2: a thread between two waits (the poller) is caught inside one: up to 40 tries, 2 ms apart.
  for (int attempt = 0; attempt < 40; ++attempt) {
    bool pending = false;
    for (const ThreadCopy& t : threads) {
      if (!t.objs_seen && t.kind != kKindSleep && t.kind != kKindYield && t.kind != kKindNone) pending = true;
    }
    if (!pending) break;
    std::this_thread::sleep_for(std::chrono::milliseconds(2));
    g_busy.store(true, std::memory_order_seq_cst);
    for (ThreadCopy& t : threads) {
      if (t.objs_seen || t.kind == kKindSleep || t.kind == kKindYield || t.kind == kKindNone) continue;
      Record& r = *t.rec;
      if (r.used.load(std::memory_order_acquire) == 2 && r.tid == t.tid && r.in_wait.load(std::memory_order_seq_cst)) {
        CaptureWait(ks, r, t, objects);
        t.in_wait = false;  // shown as caught on retry: it is not parked
        t.objs_seen = true;
      }
    }
    g_busy.store(false, std::memory_order_seq_cst);
  }

  const uint64_t now = NowNs();
  Printer out;
  char line[640];
  std::snprintf(line, sizeof(line),
                "==== snapshot #%u: no Swap for %.1f s (%llu Swaps so far), %u guest threads, %u objects ====", number,
                double(stalled_ns) / 1e9, (unsigned long long)rex::frame_chain::SwapCount(), unsigned(threads.size()),
                unsigned(objects.size()));
  out.Line(line);

  // One line per thread.
  for (const ThreadCopy& t : threads) {
    int len = std::snprintf(line, sizeof(line), "t%04X %-30s pri=%d%s | %s ", t.tid, t.name, t.priority,
                            t.main ? " MAIN" : "", t.in_wait ? "WAIT" : (t.objs_seen ? "poll" : "run "));
    auto add = [&](const char* fmt, auto... args) {
      if (len > 0 && len < int(sizeof(line))) len += std::snprintf(line + len, sizeof(line) - size_t(len), fmt, args...);
    };
    add("%s", KindName(t.kind));
    if (t.kind == kKindSleep) {
      add(" %u ms", t.keys[0]);
    } else if (t.kind == kKindCs) {
      add(" %08X", t.cs);
    } else if (t.kind != kKindYield && t.kind != kKindNone) {
      if (t.kind == kKindAny || t.kind == kKindAll) add("(%u)", t.total);
      for (uint32_t i = 0; i < t.nobj; ++i) {
        const char* role = t.kind == kKindSigWait ? (i == 0 ? " wait=" : " sig=") : " ";
        add("%s%c:%08X", role, t.types[i], t.keys[i]);
        if (t.addrs[i]) add("@%08X", t.addrs[i]);
      }
    }
    if (t.kind != kKindYield && t.kind != kKindNone) {
      if (t.timeout_ms == ~0u)
        add(" inf");
      else
        add(" to=%ums", t.timeout_ms);
      if (t.alertable) add(" alert");
    }
    char status[16];
    StatusName(t.last_status, status, sizeof(status));
    const double streak_s = t.streak_start_ns && now > t.streak_start_ns ? double(now - t.streak_start_ns) / 1e9 : 0.0;
    add(" | streak %.1f s: %u waits, %u timeouts, %u apc", streak_s, t.streak_waits, t.streak_timeouts,
        t.streak_apcs);
    if (t.last_done_ns)
      add(" | last %s %.0f ms ago", status, double(now - std::min(now, t.last_done_ns)) / 1e6);
    else
      add(" | last -");
    if (t.in_wait) add(" | in wait %.1f s", double(now - std::min(now, t.wait_start_ns)) / 1e9);
    add(" | seq %llu", (unsigned long long)t.seq);
    out.Line(line);
  }

  // One line per object in a current wait: state, who waits, last signals.
  for (size_t oi = 0; oi < objects.size(); ++oi) {
    const ObjectCopy& o = objects[oi];
    int len = std::snprintf(line, sizeof(line), "  obj %c:%08X", o.type, o.handle);
    auto add = [&](const char* fmt, auto... args) {
      if (len > 0 && len < int(sizeof(line))) len += std::snprintf(line + len, sizeof(line) - size_t(len), fmt, args...);
    };
    if (o.addr) add("@%08X", o.addr);
    if (!o.name.empty()) add(" \"%.24s\"", o.name.c_str());
    add(" | %s", o.state.empty() ? "?" : o.state.c_str());
    if (o.type == 'M') add(" owner=t%04X", o.mutant_owner);
    add(" | waiters");
    for (const ThreadCopy& t : threads) {
      for (uint32_t i = 0; i < t.nobj; ++i) {
        if (t.obj_index[i] == int(oi)) {
          add(" t%04X%s", t.tid, t.kind == kKindSigWait ? (i == 0 ? "(w)" : "(s)") : "");
        }
      }
    }
    add(" | ops(%u):", o.op_n);
    // Newest first.
    for (uint32_t k = 0; k < XObject::kSignalOps && k < o.op_n; ++k) {
      const uint32_t slot = (o.op_n - 1 - k) % XObject::kSignalOps;
      const uint64_t when = o.op_when[slot];
      const uint64_t what = o.op_what[slot];
      if (!when) continue;
      const uint32_t tid = uint32_t(what >> 32);
      const uint8_t op = uint8_t(what & 0xFF);
      const uint32_t arg = uint32_t((what >> 8) & 0xFFFFFF);
      add(" %s", OpName(op));
      if (op == kOpRelease) add("(%u,prev %u)", arg & 0xFF, arg >> 8);
      if (tid == 0xFFFFFFFFu)
        add(" host");
      else
        add(" t%04X", tid);
      add(" %.0fms;", double(now - std::min(now, when)) / 1e6);
    }
    out.Line(line);
  }

  // Critical sections.
  for (const ThreadCopy& t : threads) {
    if (!t.cs_read) continue;
    const uint32_t owner_tid = TidOfKthread(threads, t.cs_owner);
    std::snprintf(line, sizeof(line), "  cs %08X waited by t%04X | owner KTHREAD %08X = %s%04X | lock_count %d recursion %d",
                  t.cs, t.tid, t.cs_owner, owner_tid ? "t" : "unknown ", owner_tid, t.cs_lock_count, t.cs_recursion);
    out.Line(line);
  }

  // The guest main thread's last waits.
  for (const ThreadCopy& t : threads) {
    if (!t.main) continue;
    const uint32_t n = std::min(t.hist_n, kHistory);
    for (uint32_t k = 0; k < n; ++k) {
      const HistEntry& h = t.hist[(t.hist_n - n + k) % kHistory];
      char status[16];
      StatusName(h.status, status, sizeof(status));
      std::snprintf(line, sizeof(line), "  main t%04X wait -%u: %s %c:%08X %.2f ms -> %s, ended %.0f ms ago", t.tid,
                    n - k, KindName(h.kind), h.type, h.key, double(h.dur_us) / 1000.0, status,
                    double(now - std::min(now, h.end_ns)) / 1e6);
      out.Line(line);
    }
  }

  std::snprintf(line, sizeof(line), "==== end of snapshot #%u%s ====", number, out.truncated ? " (truncated)" : "");
  out.lines = 0;  // the end line always prints
  out.Line(line);
  std::fflush(stderr);
}

void WatchdogMain(KernelState* ks) {
  uint64_t printed_for = ~0ull;  // the Swap count the last snapshot was printed for
  uint64_t next_ns = 0;
  unsigned number = 0;
  for (;;) {
    std::this_thread::sleep_for(std::chrono::milliseconds(250));
    const uint64_t last = rex::frame_chain::LastSwapNs();
    if (!last) continue;  // no Swap yet (boot)
    const uint64_t count = rex::frame_chain::SwapCount();
    const uint64_t now = NowNs();
    if (now < last || now - last < kFirstNs) continue;
    if (count != printed_for) {
      printed_for = count;
      next_ns = now + kRepeatNs;
      Snapshot(ks, ++number, now - last);
    } else if (now >= next_ns) {
      next_ns = now + kRepeatNs;
      Snapshot(ks, ++number, now - last);
    }
  }
}

}  // namespace

void StartWatchdog(KernelState* kernel_state) {
  if (!Enabled() || g_watchdog_started.exchange(true)) return;
  // Never joined nor destroyed (libnx has no detach): it lives as long as the process.
  new std::thread([kernel_state] { WatchdogMain(kernel_state); });
  std::fprintf(stderr, "[hang] tekken_hang_snapshot on: a snapshot after 4 s without a Swap\n");
  std::fflush(stderr);
}

}  // namespace hang

// tekken6-nx (2026-11-21, tekken_hang_snapshot): XObject's signal record (rex/system/xobject.h).
void XObject::NoteSignalOp(uint8_t op, uint32_t arg) {
  if (!hang::Enabled()) return;
  const uint32_t tid = hang::CurrentTid();
  const uint32_t n = signal_op_n_.load(std::memory_order_relaxed);
  SignalOpRecord& slot = signal_ops_[n % kSignalOps];
  slot.what.store((uint64_t(tid) << 32) | (uint64_t(arg & 0xFFFFFF) << 8) | op, std::memory_order_relaxed);
  slot.when_ns.store(hang::NowNs(), std::memory_order_relaxed);
  signal_op_n_.store(n + 1, std::memory_order_relaxed);
}

}  // namespace rex::system
