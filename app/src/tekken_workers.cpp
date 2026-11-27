// tekken6-nx (2026-11-25): the game's job worker threads, from the Xbox 360's six hardware threads.
//
// sub_823B9ED0(slot r3, entry r4, pool r5, priority index r6, stack r7, processor r8) creates one thread, sets its
// priority from a table and its processor (0..5), and resumes it. Two pools call it in a loop for their workers:
// sub_823BA228 (3 workers) and sub_823B9FA8 (2 workers), both with the entry sub_823B9E28. That entry is a plain job
// queue: it takes a job from the pool (virtual +16, which waits on the pool's semaphore), runs it (job virtual +4),
// and signals its group (sub_823BA458) when it has one; so any worker can take any job.
//
// The idea (a forum note about Dark Souls 2's port: 5 Havok + 5 worker threads inherited from the Xbox 360 fought for
// the CPU; 2 + 3 ran better): the Switch has four cores for the game, its main thread and Renut's render thread, so
// five workers may only take CPU time from them. tekken_workers_per_pool caps each pool's workers (0 = the game's
// own count); the others are not created, so their slots keep a null handle. Every creation is logged once on stderr
// ([workers]) with its pool, processor and priority index.
//
// Console result (2026-11-26, packages W0 / W1 / W2, Cemetery at 1020 MHz): a cap of 2 hung after one fight (some job
// waits for a worker that is not there, or two jobs wait on each other), and 4 gave no measurable fps over the game's
// own five. Kept as a diagnostic: the default 0 leaves the game's count.
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <mutex>

#include <rex/cvar.h>
#include <rex/hook.h>
#include <rex/logging.h>
#include <rex/ppc.h>

REXCVAR_DEFINE_INT32(tekken_workers_per_pool, 0, "Tekken6",
                     "The most job worker threads each of the game's two worker pools starts (the game starts 3 and "
                     "2); 0 = the game's own count")
    .range(0, 8)
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);

REX_EXTERN(__imp__sub_823B9ED0);

namespace {
constexpr uint32_t kWorkerEntry = 0x823B9E28u;

struct PoolCount {
  uint32_t pool = 0;
  int created = 0;
  int skipped = 0;
};
std::mutex g_mutex;
PoolCount g_pools[8];

void StoreU32(uint8_t* base, uint32_t va, uint32_t value) {
  value = __builtin_bswap32(value);
  std::memcpy(base + va, &value, 4);
}
}  // namespace

REX_HOOK_RAW(sub_823B9ED0) {
  const uint32_t slot = ctx.r3.u32, entry = ctx.r4.u32, pool = ctx.r5.u32, priority_index = ctx.r6.u32;
  const int32_t processor = ctx.r8.s32;
  if (entry != kWorkerEntry) {
    __imp__sub_823B9ED0(ctx, base);
    return;
  }
  static const int32_t cap = REXCVAR_GET(tekken_workers_per_pool);
  int index = 0;
  bool skip = false;
  {
    std::lock_guard lock(g_mutex);
    PoolCount* p = nullptr;
    for (auto& candidate : g_pools)
      if (candidate.pool == pool || candidate.pool == 0) {
        p = &candidate;
        break;
      }
    if (p) {
      p->pool = pool;
      index = p->created + p->skipped;
      skip = cap > 0 && p->created >= cap;
      (skip ? p->skipped : p->created)++;
    }
  }
  if (skip) {
    // What sub_823B9ED0 stores in the slot, without a thread: +0 handle (null), +4 entry, +8 pool.
    StoreU32(base, slot + 0, 0);
    StoreU32(base, slot + 4, entry);
    StoreU32(base, slot + 8, pool);
    ctx.r3.u64 = 0;
  } else {
    __imp__sub_823B9ED0(ctx, base);
  }
  std::fprintf(stderr, "[workers] pool %08X worker %d: processor %d, priority index %u: %s (tekken_workers_per_pool %d)\n",
               pool, index, processor, priority_index, skip ? "NOT started" : "started", cap);
  REXLOG_INFO("[workers] pool {:08X} worker {}: processor {}, priority index {}: {}", pool, index, processor,
              priority_index, skip ? "NOT started" : "started");
}
