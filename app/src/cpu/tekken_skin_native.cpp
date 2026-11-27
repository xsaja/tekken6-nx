// tekken6-nx (2026-11-10): native version of sub_82548758, the hottest function of the first console profile (37 % of
// the six job-pool threads' time in a fight, out/console_runs/console1). It is a job the pool calls through the
// dispatch table (no direct caller), so a plain hook replaces it.
//
// The job (r3) holds: +4 table B, +8 table A, +12 (u16) the first matrix index, +16 output, +20 records, +24 count.
// Each 96-byte record is T (+0), W (+16), P (+32), Q (+48), four matrix indices (+64), weights S (+80); each makes
// four output vectors (64 bytes), with M_k = the k-th indexed 64-byte matrix (rows M[0..3]):
//   out+0  = sum_k S_k * (((B_k[0]*T0 + B_k[3]) + B_k[1]*T1) + B_k[2]*T2)
//   out+16 = sum_k S_k * ((A_k[0]*W0 + A_k[1]*W1) + A_k[2]*W2)        (same for P at +32 and Q at +48)
// with every sum taken in k order: ((t0*S0 + t1*S1) + t2*S2) + t3*S3. The original's operations exactly: separate
// multiplies and adds (vmulfp128 / vmaddfp as the generated code computes them; -ffp-contract=off on the target), in
// the same association, under the same flush mode, so the stored bytes are the same. The generated loop runs 368 A57
// instructions a record (68 loads for 34 vectors, 16 stack spills); this one loads each vector once and multiplies by
// lane. The timer calls around the loop (sub_8254A3C8 / sub_8254A400, the game's own job timing) are kept as they are.
//
// tekken6-nx (2026-11-10, second console profile): sub_825481F8, the next job of the workers (10.6 %), is the same
// blend with only the T and W outputs (64-byte records: T +0, W +16, indices +32, S +48; 32 bytes out); one kernel
// (Run<kFull>) serves both, hooked the same way (only called through the dispatch table).
//
// tekken_native_skin_check compares every call with the original (it runs both): for PC and Ryujinx tests only.
//
// tekken6-nx (2026-11-20 night): the console profile out/console_runs/dieta_2223 put 41 % of this function's samples
// (3,256 of 7,890) on one instruction, the loop's first load (the record's matrix indices at +64): the record stream
// misses the caches (each record is read once a frame, 96 bytes apart; the A57's prefetcher does not keep ahead of
// it). The loop now prefetches the records tekken_native_skin_prefetch records ahead (two lines a 96-byte record,
// one a 64-byte one); nothing else changes, so the stored bytes are the same. tekken_native_skin_prefetch_out also
// prefetches the output lines for writing (off by default: untested on the console).
// tekken_native_skin_check_control: the check with the original on both sides (the harness's own noise).
//
// tekken6-nx (2026-11-27, console profile prueba_T_perfil_skinning): with the prefetch the record loads no longer
// stand out; the samples spread over the loop's 288 instructions a record, 37 of them byte swaps and 33 address masks
// of the four bone matrices (7 vectors each, loaded and swapped again for every record that names the bone), and 17
// stack spills. tekken_native_skin_bones: a pre-pass over the job's records swaps every named bone's 7 vectors once
// into a per-thread table (the same 16-byte-aligned guest vectors, the same swap: the same values), and the loop
// reads them from there: 187 instructions a record, 8 spills, the 112 multiplies and adds untouched (same order). The
// table has 256 slots by index minus the job's first index; a job naming an index outside them runs the old loop.
// Checked against the old loop on 800 random jobs (a standalone host test) and in Ryujinx with
// tekken_native_skin_check: 89 million records, 0 differ, no job past the table.
// Console A/B (Cemetery, 1020 MHz, sampling on in both): the first version (no prefetch in the pre-pass) lost 3.4 fps
// (the pre-pass paid a cache miss a record); with the pre-pass prefetch the kernel's samples fell 24 % (91 -> 69 a
// 10 s report) and the fight went from 56.4 to 57.2 fps. On by default.
#include <rex/cvar.h>
#include <rex/hook.h>
#include <rex/logging.h>
#include <rex/ppc.h>
#include <rex/system/xmemory.h>

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>

#include "renut_engine/gpu/render/render_census.h"

#if defined(__aarch64__)
#include <arm_neon.h>
#else
#include <simde/x86/ssse3.h>
#endif

REXCVAR_DEFINE_BOOL(tekken_native_skin, true, "Tekken6/CPU",
                    "Native version of the matrix blend job sub_82548758 (character skinning); same results")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_BOOL(tekken_native_skin_check, false, "Tekken6/CPU",
                    "Run the original after the native version on every call and log any difference (tests only)")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_BOOL(tekken_native_skin_check_control, false, "Tekken6/CPU",
                    "With tekken_native_skin_check: run the original on both sides (measures the check's own noise)")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_INT32(tekken_native_skin_prefetch, 8, "Tekken6/CPU",
                     "Native skinning: prefetch the input records this many records ahead (0 = no prefetch)")
    .range(0, 64)
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_INT32(tekken_native_skin_prefetch_out, 0, "Tekken6/CPU",
                     "Native skinning: prefetch the output lines for writing this many records ahead (0 = no)")
    .range(0, 64)
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_BOOL(tekken_native_skin_bones, true, "Tekken6/CPU",
                    "Native skinning: byte-swap each bone matrix once a job instead of once a record (same results)")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);

REX_EXTERN(__imp__sub_82548758);
REX_EXTERN(__imp__sub_825481F8);
REX_EXTERN(__imp__sub_8254A3C8);
REX_EXTERN(__imp__sub_8254A400);

namespace {

inline uint32_t Load32(const uint8_t* base, uint32_t va) {
  uint32_t w;
  std::memcpy(&w, base + va, 4);
  return __builtin_bswap32(w);
}

// A guest vector of four floats in guest lane order (lane i = the word at +4i), and back.
#if defined(__aarch64__)
using V = float32x4_t;
inline V Load(const uint8_t* base, uint32_t va) {
  return vreinterpretq_f32_u8(vrev32q_u8(vld1q_u8(base + (va & ~0xFu))));
}
inline void Store(uint8_t* p, V v) { vst1q_u8(p, vrev32q_u8(vreinterpretq_u8_f32(v))); }
inline V Add(V a, V b) { return vaddq_f32(a, b); }
template <int L>
inline V MulLane(V a, V b) {
  return vmulq_laneq_f32(a, b, L);
}
#else
using V = simde__m128;
inline simde__m128i Swap32(simde__m128i v) {
  return simde_mm_shuffle_epi8(v, simde_mm_set_epi8(12, 13, 14, 15, 8, 9, 10, 11, 4, 5, 6, 7, 0, 1, 2, 3));
}
inline V Load(const uint8_t* base, uint32_t va) {
  return simde_mm_castsi128_ps(Swap32(simde_mm_loadu_si128(reinterpret_cast<const simde__m128i*>(base + (va & ~0xFu)))));
}
inline void Store(uint8_t* p, V v) {
  simde_mm_storeu_si128(reinterpret_cast<simde__m128i*>(p), Swap32(simde_mm_castps_si128(v)));
}
inline V Add(V a, V b) { return simde_mm_add_ps(a, b); }
template <int L>
inline V MulLane(V a, V b) {
  return simde_mm_mul_ps(a, simde_mm_shuffle_ps(b, b, SIMDE_MM_SHUFFLE(L, L, L, L)));
}
#endif

// (m0*v.x + m1*v.y) + m2*v.z, the original's order.
inline V Rows3(V m0, V m1, V m2, V v) { return Add(Add(MulLane<0>(m0, v), MulLane<1>(m1, v)), MulLane<2>(m2, v)); }

// Matrix k of a record: its terms, added to the blends ((t0*s0 + t1*s1) + t2*s2) + t3*s3 in k order (no arrays:
// GCC kept arrays of vectors on the stack and cleared them with a memset call a record). kFull: also P and Q.
template <bool kFull, int K>
[[gnu::always_inline]] inline void Matrix(const uint8_t* base, uint32_t index_va, uint32_t table_a, uint32_t table_b,
                                          V t, V w, V p, V q, V s, V& u, V& x, V& y, V& z) {
  const uint32_t index = Load32(base, index_va + 4 * K) << 6;
  const uint32_t a = index + table_a, b = index + table_b;
  const V a0 = Load(base, a), a1 = Load(base, a + 16), a2 = Load(base, a + 32);
  const V b0 = Load(base, b), b1 = Load(base, b + 16), b2 = Load(base, b + 32), b3 = Load(base, b + 48);
  const V uk = Add(Add(Add(MulLane<0>(b0, t), b3), MulLane<1>(b1, t)), MulLane<2>(b2, t));
  const V xk = Rows3(a0, a1, a2, w);
  if constexpr (K == 0) {
    u = MulLane<0>(uk, s);
    x = MulLane<0>(xk, s);
  } else {
    u = Add(u, MulLane<K>(uk, s));
    x = Add(x, MulLane<K>(xk, s));
  }
  if constexpr (kFull) {
    const V yk = Rows3(a0, a1, a2, p), zk = Rows3(a0, a1, a2, q);
    if constexpr (K == 0) {
      y = MulLane<0>(yk, s);
      z = MulLane<0>(zk, s);
    } else {
      y = Add(y, MulLane<K>(yk, s));
      z = Add(z, MulLane<K>(zk, s));
    }
  }
}

// tekken_native_skin_bones (2026-11-27): the job's bone matrices swapped once. Slot = index - the job's first index
// (the tables start there), tagged with the job's generation (no clearing between jobs). 128 bytes a slot (a shift).
#if defined(__aarch64__)
inline V LoadHost(const float* p) { return vld1q_f32(p); }
inline void StoreHost(float* p, V v) { vst1q_f32(p, v); }
#else
inline V LoadHost(const float* p) { return simde_mm_loadu_ps(p); }
inline void StoreHost(float* p, V v) { simde_mm_storeu_ps(p, v); }
#endif

constexpr uint32_t kBoneSlots = 256;
struct Bone {
  float a[3][4];
  float b[4][4];
  float pad[4];
};
struct BoneTable {
  uint32_t gen[kBoneSlots] = {};
  Bone bone[kBoneSlots];
  uint32_t current = 0;
};
std::atomic<uint64_t> g_bone_jobs{0}, g_bone_fallbacks{0};

BoneTable& ThreadBones() {
  thread_local BoneTable* table = new BoneTable();  // ~33 KB a worker thread, once
  return *table;
}

// Swaps the 7 vectors of every bone the job's records name into the table. False when an index falls outside the
// 256 slots (the caller then runs the old loop).
template <bool kFull>
bool PrepareBones(BoneTable& c, const uint8_t* base, uint32_t rec, int32_t count, uint32_t first_raw,
                  uint32_t table_a, uint32_t table_b) {
  constexpr uint32_t kRecord = kFull ? 96 : 64, kIndex = kFull ? 64 : 32;
  const uint32_t gen = ++c.current;
  // 2026-11-27 (console A/B prueba_U): without this the pass took a full cache miss a record (the records are cold;
  // the A57's prefetcher does not keep up with them): the workers' CPU went from 8.4 to 13.7 ms a frame. All the
  // record's lines, so the main loop then finds them in the cache too.
  constexpr uint32_t kAhead = 16 * kRecord;
  for (int32_t j = 0; j < count; ++j, rec += kRecord) {
    const uint8_t* ahead = base + uint32_t(rec + kAhead);
    __builtin_prefetch(ahead, 0, 3);
    __builtin_prefetch(ahead + 64, 0, 3);
    if constexpr (kFull) __builtin_prefetch(ahead + 95, 0, 3);
    for (uint32_t k = 0; k < 4; ++k) {
      const uint32_t raw = Load32(base, rec + kIndex + 4 * k);
      const uint32_t slot = raw - first_raw;
      if (slot >= kBoneSlots) return false;
      if (c.gen[slot] == gen) continue;
      const uint32_t index = raw << 6;
      const uint32_t a = index + table_a, b = index + table_b;
      Bone& o = c.bone[slot];
      for (int r = 0; r < 3; ++r) StoreHost(o.a[r], Load(base, a + 16 * r));
      for (int r = 0; r < 4; ++r) StoreHost(o.b[r], Load(base, b + 16 * r));
      c.gen[slot] = gen;
    }
  }
  return true;
}

// Matrix k from the table: the same vectors and the same operations as Matrix.
template <bool kFull, int K>
[[gnu::always_inline]] inline void BoneMatrix(const Bone& m, V t, V w, V p, V q, V s, V& u, V& x, V& y, V& z) {
  const V a0 = LoadHost(m.a[0]), a1 = LoadHost(m.a[1]), a2 = LoadHost(m.a[2]);
  const V b0 = LoadHost(m.b[0]), b1 = LoadHost(m.b[1]), b2 = LoadHost(m.b[2]), b3 = LoadHost(m.b[3]);
  const V uk = Add(Add(Add(MulLane<0>(b0, t), b3), MulLane<1>(b1, t)), MulLane<2>(b2, t));
  const V xk = Rows3(a0, a1, a2, w);
  if constexpr (K == 0) {
    u = MulLane<0>(uk, s);
    x = MulLane<0>(xk, s);
  } else {
    u = Add(u, MulLane<K>(uk, s));
    x = Add(x, MulLane<K>(xk, s));
  }
  if constexpr (kFull) {
    const V yk = Rows3(a0, a1, a2, p), zk = Rows3(a0, a1, a2, q);
    if constexpr (K == 0) {
      y = MulLane<0>(yk, s);
      z = MulLane<0>(zk, s);
    } else {
      y = Add(y, MulLane<K>(yk, s));
      z = Add(z, MulLane<K>(zk, s));
    }
  }
}

// The job's loop: writes each record's output (64 or 32 bytes) to out(j) (guest memory, or a buffer for the check).
// kFull (sub_82548758): 96-byte records T, W, P, Q, indices +64, S +80. Else (sub_825481F8): 64-byte records T, W,
// indices +32, S +48, and only the T and W outputs.
// Prefetches (2026-11-20): `ahead` records ahead of the one being read (0 = none), and the output `ahead_out`
// records ahead for writing (out_va: the guest output address and its record size; 0 = none). A prefetch never
// faults (past the end, or on an unmapped page, it is dropped) and changes no value.
struct Prefetch {
  uint32_t ahead = 0, ahead_out = 0, out_va = 0, out_bytes = 0;
};

template <bool kFull, class Out>
inline void Run(const uint8_t* base, uint32_t job, Out out, const Prefetch& pf = {}, bool bones = false) {
  constexpr uint32_t kRecord = kFull ? 96 : 64, kIndex = kFull ? 64 : 32, kWeights = kFull ? 80 : 48;
  const int32_t count = int32_t(Load32(base, job + 24));
  if (count <= 0) return;
  const uint32_t first_raw = Load32(base, job + 12) >> 16;  // lhz at +12
  const uint32_t first = first_raw << 6;
  const uint32_t table_a = Load32(base, job + 8) - first;
  const uint32_t table_b = Load32(base, job + 4) - first;
  uint32_t rec = Load32(base, job + 20);
  const uint32_t pf_step = pf.ahead * kRecord;
  const uint32_t pf_out_step = pf.ahead_out * pf.out_bytes;
  uint32_t pf_out = pf.out_va;
  if (bones) {
    BoneTable& c = ThreadBones();
    g_bone_jobs.fetch_add(1, std::memory_order_relaxed);
    if (PrepareBones<kFull>(c, base, rec, count, first_raw, table_a, table_b)) {
      const Bone* table = c.bone;
      for (int32_t j = 0; j < count; ++j, rec += kRecord) {
        if (pf_step != 0) {
          const uint8_t* p = base + uint32_t(rec + pf_step);
          __builtin_prefetch(p, 0, 3);
          if constexpr (kFull) __builtin_prefetch(p + 64, 0, 3);
        }
        if (pf_out_step != 0) {
          const uint8_t* p = base + uint32_t(pf_out + pf_out_step);
          __builtin_prefetch(p, 1, 3);
          if constexpr (kFull) __builtin_prefetch(p + 48, 1, 3);
          pf_out += pf.out_bytes;
        }
        const V t = Load(base, rec), w = Load(base, rec + 16), s = Load(base, rec + kWeights);
        V p = t, q = t, u, x, y = t, z = t;
        if constexpr (kFull) {
          p = Load(base, rec + 32);
          q = Load(base, rec + 48);
        }
        // In range: PrepareBones checked every index of the job.
        const uint32_t i0 = Load32(base, rec + kIndex) - first_raw, i1 = Load32(base, rec + kIndex + 4) - first_raw;
        const uint32_t i2 = Load32(base, rec + kIndex + 8) - first_raw, i3 = Load32(base, rec + kIndex + 12) - first_raw;
        BoneMatrix<kFull, 0>(table[i0 & (kBoneSlots - 1)], t, w, p, q, s, u, x, y, z);
        BoneMatrix<kFull, 1>(table[i1 & (kBoneSlots - 1)], t, w, p, q, s, u, x, y, z);
        BoneMatrix<kFull, 2>(table[i2 & (kBoneSlots - 1)], t, w, p, q, s, u, x, y, z);
        BoneMatrix<kFull, 3>(table[i3 & (kBoneSlots - 1)], t, w, p, q, s, u, x, y, z);
        uint8_t* d = out(j);
        Store(d, u);
        Store(d + 16, x);
        if constexpr (kFull) {
          Store(d + 32, y);
          Store(d + 48, z);
        }
      }
      return;
    }
    g_bone_fallbacks.fetch_add(1, std::memory_order_relaxed);
  }
  for (int32_t j = 0; j < count; ++j, rec += kRecord) {
    if (pf_step != 0) {
      // A 96-byte record at any offset: the lines of +0 and +64 here, the third (if any) by the next record's +0.
      const uint8_t* p = base + uint32_t(rec + pf_step);
      __builtin_prefetch(p, 0, 3);
      if constexpr (kFull) __builtin_prefetch(p + 64, 0, 3);
    }
    if (pf_out_step != 0) {
      const uint8_t* p = base + uint32_t(pf_out + pf_out_step);
      __builtin_prefetch(p, 1, 3);
      if constexpr (kFull) __builtin_prefetch(p + 48, 1, 3);  // its 16-byte-aligned last vector
      pf_out += pf.out_bytes;
    }
    const V t = Load(base, rec), w = Load(base, rec + 16), s = Load(base, rec + kWeights);
    V p = t, q = t, u, x, y = t, z = t;
    if constexpr (kFull) {
      p = Load(base, rec + 32);
      q = Load(base, rec + 48);
    }
    Matrix<kFull, 0>(base, rec + kIndex, table_a, table_b, t, w, p, q, s, u, x, y, z);
    Matrix<kFull, 1>(base, rec + kIndex, table_a, table_b, t, w, p, q, s, u, x, y, z);
    Matrix<kFull, 2>(base, rec + kIndex, table_a, table_b, t, w, p, q, s, u, x, y, z);
    Matrix<kFull, 3>(base, rec + kIndex, table_a, table_b, t, w, p, q, s, u, x, y, z);
    uint8_t* d = out(j);
    Store(d, u);
    Store(d + 16, x);
    if constexpr (kFull) {
      Store(d + 32, y);
      Store(d + 48, z);
    }
  }
}

// One job's fixed values: output bytes a record, the thread block offsets its two timer reads go to, the return
// addresses of its three calls (the generated code's ctx.lr), its original.
struct Job {
  uint32_t out_bytes, start_at, end_at, lr_start, lr_end, lr_report;
  PPCFunc* original;
  const char* name;
};

std::atomic<uint64_t> g_checked{0}, g_differ{0}, g_records{0};

// The original around its loop: the job timer read before and after (stored in the thread block) and handed to
// sub_8254A400 as the generated code does, with the registers it leaves set for that call.
template <bool kFull>
void Native(PPCContext& __restrict ctx, uint8_t* base, const Job& job_info) {
  const uint32_t job = ctx.r3.u32;
  constexpr uint32_t kTimer = 0x829CA000;  // lis r11,-32099; addi rN,r11,-24576
  const uint32_t block = Load32(base, ctx.r13.u32);
  ctx.r3.u64 = kTimer;
  ctx.lr = job_info.lr_start;
  __imp__sub_8254A3C8(ctx, base);
  rex::memory::store_and_swap<uint64_t>(base + block + job_info.start_at, ctx.r3.u64);
  rex::memory::MarkStoreCardsAt(base, block + job_info.start_at, 8);

  const int32_t count = int32_t(Load32(base, job + 24));
  if (count > 0) {
    ctx.fpscr.enableFlushMode();  // as the original's first vector multiply
    const uint32_t out = Load32(base, job + 16);
    static const uint32_t ahead = uint32_t(REXCVAR_GET(tekken_native_skin_prefetch));
    static const uint32_t ahead_out = uint32_t(REXCVAR_GET(tekken_native_skin_prefetch_out));
    static const bool bones = REXCVAR_GET(tekken_native_skin_bones);
    const Prefetch pf{ahead, ahead_out, out, job_info.out_bytes};
    Run<kFull>(base, job, [&](int32_t j) {
      const uint32_t va = (out + job_info.out_bytes * uint32_t(j)) & ~0xFu;
      // The store cards of the 16-byte stores (REX_CARD: their first byte's page): the first's and the last's.
      // Inline: MarkStoreCardsAt's loop became a memset call a record.
      base[va >> 12] = 0;
      base[(va + job_info.out_bytes - 16) >> 12] = 0;
      return base + va;
    }, pf, bones);
    // stderr: rex_stderr.log on the Switch. How often a job fell back to the old loop (an index past the table).
    if (bones) {
      const uint64_t n = g_bone_jobs.load(std::memory_order_relaxed);
      if (n % 200000 == 0)
        std::fprintf(stderr, "[tekken_native_skin] bones: %llu jobs, %llu ran the old loop (index past 256 slots)\n",
                     (unsigned long long)n, (unsigned long long)g_bone_fallbacks.load(std::memory_order_relaxed));
    }
    if (renut::gpu::render_census::Enabled())  // tekken6-nx (2026-11-23): which conversions read skinned vertices
      renut::gpu::render_census::NoteSkinOutput(base + (out & ~0xFu), uint32_t(count) * job_info.out_bytes);
  }

  ctx.r3.u64 = kTimer;
  ctx.lr = job_info.lr_end;
  __imp__sub_8254A3C8(ctx, base);
  const uint64_t end = ctx.r3.u64;
  rex::memory::store_and_swap<uint64_t>(base + block + job_info.end_at, end);
  rex::memory::MarkStoreCardsAt(base, block + job_info.end_at, 8);
  if constexpr (kFull) {  // li r10,304; mr r9,r3; li r11,312
    ctx.r9.u64 = end;
    ctx.r10.s64 = job_info.end_at;
    ctx.r11.s64 = job_info.start_at;
  } else {  // li r11,272; mr r10,r3
    ctx.r10.u64 = end;
    ctx.r11.s64 = job_info.end_at;
  }
  ctx.r3.u64 = kTimer;
  ctx.r5.u64 = end;
  ctx.r4.u64 = rex::memory::load_and_swap<uint64_t>(base + block + job_info.start_at);
  ctx.lr = job_info.lr_report;
  __imp__sub_8254A400(ctx, base);
}

// Both: the native result into a buffer, then the original into guest memory, compared. The control
// (tekken_native_skin_check_control): the original twice instead (the first copied out, the registers set back).
template <bool kFull>
void Check(PPCContext& __restrict ctx, uint8_t* base, const Job& job_info) {
  static const bool control = REXCVAR_GET(tekken_native_skin_check_control);
  static const uint32_t ahead = uint32_t(REXCVAR_GET(tekken_native_skin_prefetch));
  static const uint32_t ahead_out = uint32_t(REXCVAR_GET(tekken_native_skin_prefetch_out));
  const uint32_t job = ctx.r3.u32;
  const int32_t count = int32_t(Load32(base, job + 24));
  const uint32_t out = Load32(base, job + 16);
  const uint32_t bytes = job_info.out_bytes;
  std::vector<uint8_t> mine(count > 0 ? size_t(count) * bytes : 0);
  if (control) {
    const PPCContext saved = ctx;
    job_info.original(ctx, base);
    for (int32_t j = 0; j < count; ++j)
      std::memcpy(mine.data() + size_t(j) * bytes, base + ((out + bytes * uint32_t(j)) & ~0xFu), bytes);
    ctx = saved;
    rex::platform::FPSCRPlatform::setcsr(ctx.fpscr.csr);
  } else if (count > 0) {
    ctx.fpscr.enableFlushMode();
    static const bool bones = REXCVAR_GET(tekken_native_skin_bones);
    const Prefetch pf{ahead, ahead_out, out, bytes};
    Run<kFull>(base, job, [&](int32_t j) { return mine.data() + size_t(j) * bytes; }, pf, bones);
  }
  job_info.original(ctx, base);
  const uint64_t n = g_checked.fetch_add(1, std::memory_order_relaxed) + 1;
  if (count > 0) g_records.fetch_add(uint64_t(count), std::memory_order_relaxed);
  for (int32_t j = 0; j < count; ++j) {
    const uint8_t* theirs = base + ((out + bytes * uint32_t(j)) & ~0xFu);
    if (std::memcmp(mine.data() + size_t(j) * bytes, theirs, bytes) != 0) {
      if (g_differ.fetch_add(1, std::memory_order_relaxed) < 8)
        std::fprintf(stderr, "[tekken_native_skin] DIFFERENCE: %s job %08X record %d of %d\n", job_info.name, job, j,
                     count);
    }
  }
  // stderr: rex_stderr.log on the Switch, where the built-in settings turn the log off.
  if (n % 2000 == 0)
    std::fprintf(stderr, "[tekken_native_skin] checked %llu calls (%llu records)%s, %llu records differ; bones: %llu "
                 "jobs, %llu ran the old loop\n",
                 (unsigned long long)n, (unsigned long long)g_records.load(), control ? " [control]" : "",
                 (unsigned long long)g_differ.load(), (unsigned long long)g_bone_jobs.load(),
                 (unsigned long long)g_bone_fallbacks.load());
}

constexpr Job kFullJob = {64, 312, 304, 0x8254878C, 0x82548AC4, 0x82548AE4, __imp__sub_82548758, "sub_82548758"};
constexpr Job kHalfJob = {32, 280, 272, 0x8254822C, 0x82548434, 0x82548450, __imp__sub_825481F8, "sub_825481F8"};

template <bool kFull>
void Dispatch(PPCContext& __restrict ctx, uint8_t* base, const Job& job) {
  static const bool on = REXCVAR_GET(tekken_native_skin);
  static const bool check = REXCVAR_GET(tekken_native_skin_check);
  if (check) {
    Check<kFull>(ctx, base, job);
  } else if (on) {
    Native<kFull>(ctx, base, job);
  } else {
    job.original(ctx, base);
  }
}

}  // namespace

REX_HOOK_RAW(sub_82548758) { Dispatch<true>(ctx, base, kFullJob); }
REX_HOOK_RAW(sub_825481F8) { Dispatch<false>(ctx, base, kHalfJob); }
