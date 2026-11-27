// tekken6-nx (2026-11-19 night): native versions of the two particle-update functions that dominate the rain / snow
// stages (console profiles out/console_runs/rain_1123 and copias_2230: 14-24 % and 6-7 % of the worker threads'
// samples): sub_822E05C0 (one emitter kind, called from sub_822E1AA0) and sub_822E10F0 (another, from sub_822E2B40).
// Each walks the emitter's particles; per particle it updates position / velocity / life, then builds a world matrix
// in the guest's VMX "matrix registers" v92..v95 (push, identity, translate, multiply by the emitter matrix, three
// rotations by angle in the order the particle's flags give, scale), transforms the four corners of a quad and writes
// four 48-byte vertices into a slot taken from a global counter (atomic, shared by the workers).
//
// Technique: the generated code (app/generated/renut_recomp.11.cpp / .12.cpp and its callees) transliterated with
// host locals: every floating-point expression is the generated one (same operations, same operand order, same
// single/double roundings, the same simde vector calls for the VMX parts), in the same flush mode (the 360's VMX
// flushes denormals; the scalar FPU does not), so the stored bytes are the same. What goes: the PPCContext register
// traffic, the guest stack temporaries, the calls of the small callees (vector add/sub/scale, the sin/cos table, the
// atan2 of the CRT, the matrix-register helpers), the flush-mode switches between them (two or three per particle
// instead of about a dozen: the vector work of a particle is grouped, and a 3-lane add/sub of the update step runs in
// the scalar mode when no input or result is denormal, where the two modes give the same result; otherwise in the
// vector mode as the original). Rare or large callees stay calls of the generated code with the registers the
// original passes: sub_822E00C0 (respawn), sub_822B71B8 (an optional force field).
//
// Effects the callers and the rest of the game see, as the original: guest memory (particle records, vertex slots,
// the slot table of sub_822E05C0, the counters, the RNG through the respawn, the thread's matrix stack, store
// cards), v92..v95 / v127 unchanged, r1, the final flush mode, r3 = 0 for sub_822E10F0 (its callers read nothing
// else). Volatile registers are not reproduced (both callers are ABI callers that read none of them).
//
// tekken_native_particles_check: runs the original, then the native on the restored inputs with the counter values
// the original took (no second allocation), and compares every byte both write; tests only (PC / Ryujinx).
#include <rex/cvar.h>
#include <rex/hook.h>
#include <rex/logging.h>
#include <rex/ppc.h>
#include <rex/ppc/diet_ops.h>

#include <simde/x86/avx.h>
#include <simde/x86/sse4.1.h>

#include <atomic>
#include <bit>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <vector>

// tekken6-nx (2026-11-20): default OFF. On the console the build with it on froze mid-fight on a rain stage (every guest
// thread parked in sub_824BD200 SignalObjectAndWait, the job pool's lost wake-up family); with it off, 7.5 min on rain
// and snow stages ran clean at the same ~42 fps, so it bought nothing measurable there (docs/switch-port-log.md).
// 2026-11-23: that freeze was the RainCondvar pulse race (thread_pulse_latch_names), not this code; with the latch in
// place it measured +4.7 / +5.3 fps, and the Switch build turns it on in app/katamari_switch_defaults.toml.
#include "renut_engine/gpu/render/render_census.h"

REXCVAR_DEFINE_BOOL(tekken_native_particles, false, "Tekken6/CPU",
                    "Native versions of the particle updates sub_822E05C0 / sub_822E10F0 (rain, snow); same results")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_BOOL(tekken_native_particles_check_control, false, "Tekken6/CPU",
                    "With tekken_native_particles_check: compare the original with itself (the harness's own noise)")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_BOOL(tekken_native_particles_check, false, "Tekken6/CPU",
                    "Run the original and the native particle updates on every call and log any difference (tests)")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);

// tekken6-nx (2026-11-24): the console profile (Cemetery, rain, three worker threads at ~16 % of a core each, two
// thirds of it in Native05) put the hottest instruction of Native05 on each particle's first load (its life counter):
// the records are walked once a frame, 144 / 160 bytes apart, and miss the caches. Prefetching the records this many
// particles ahead changes no result.
REXCVAR_DEFINE_INT32(tekken_native_particles_prefetch, 4, "Tekken6/CPU",
                     "Native particle updates: prefetch the particle records this many particles ahead (0 = off)")
    .range(0, 32)
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);

REX_EXTERN(__imp__sub_822E05C0);
REX_EXTERN(__imp__sub_822E10F0);
REX_EXTERN(__imp__sub_822E00C0);
REX_EXTERN(__imp__sub_822B71B8);

namespace {

using u32 = uint32_t;
using u64 = uint64_t;
using V = simde__m128;

// ---------------------------------------------------------------------------------------------------------------
// Guest memory, as the generated macros access it (big-endian words; the PC's physical window offset; store cards).
struct Mem {
  uint8_t* base;
  uint8_t* At(u32 a) const {
#if defined(_WIN32)
    return base + a + (a >= 0xE0000000u ? 0x1000u : 0u);
#else
    return base + a;
#endif
  }
  u32 U32(u32 a) const {
    u32 w;
    std::memcpy(&w, At(a), 4);
    return __builtin_bswap32(w);
  }
  u64 U64(u32 a) const {
    u64 w;
    std::memcpy(&w, At(a), 8);
    return __builtin_bswap64(w);
  }
  float F32(u32 a) const { return std::bit_cast<float>(U32(a)); }
  double D(u32 a) const { return double(F32(a)); }  // lfs (flush mode off)
  void Card(u32 a) const { base[a >> 12] = 0; }
  void SetU32(u32 a, u32 v) const {
    const u32 w = __builtin_bswap32(v);
    std::memcpy(At(a), &w, 4);
    Card(a);
  }
  void SetF(u32 a, double d) const { SetU32(a, std::bit_cast<u32>(float(d))); }  // stfs: temp.f32 = float(f)
  // lvx: the aligned 16 bytes, as the generated code holds a guest vector (host lane i = guest lane 3 - i).
  V LoadA(u32 a) const { return Rev(At(a & ~0xFu)); }
  // lvlx(a) | lvrx(a + 16): the 16 bytes at a.
  V LoadU(u32 a) const { return Rev(At(a)); }
  void StoreA(u32 a, V v) const {  // stvx (REX_CARD of the aligned address)
    const u32 ea = a & ~0xFu;
    Card(ea);
    simde_mm_storeu_si128(reinterpret_cast<simde__m128i*>(At(ea)),
                          simde_mm_shuffle_epi8(simde_mm_castps_si128(v), Mask()));
  }
  static simde__m128i Mask() { return simde_mm_loadu_si128(reinterpret_cast<const simde__m128i*>(VectorMaskL)); }
  static V Rev(const uint8_t* p) {
    return simde_mm_castsi128_ps(
        simde_mm_shuffle_epi8(simde_mm_loadu_si128(reinterpret_cast<const simde__m128i*>(p)), Mask()));
  }
};

// The generated code's single-precision macros (tools/single_floats.py, on in every build).
inline double FMULS(double a, double b) { return double(float(a) * float(b)); }
inline double FSUBS(double a, double b) { return double(float(a) - float(b)); }
inline double FDIVS(double a, double b) { return double(float(a) / float(b)); }
inline double FSQRTS(double a) { return double(__builtin_sqrtf(float(a))); }
inline double RO(u32 bits) { return double(std::bit_cast<float>(bits)); }  // lfs of a folded constant
inline u32 Quiet(u32 bits) { return rex::ppc::QuietSingleBits(bits); }
inline u32 Bits(V v, int host_lane) {
  alignas(16) u32 l[4];
  simde_mm_store_si128(reinterpret_cast<simde__m128i*>(l), simde_mm_castps_si128(v));
  return l[host_lane];
}

// ---------------------------------------------------------------------------------------------------------------
// The VMX helpers of the matrix registers (sub_825133xx..825136xx, 82515528, 825154E8), as their generated code.
inline V Mul(V a, V b) { return simde_mm_mul_ps(a, b); }
inline V Add(V a, V b) { return simde_mm_add_ps(a, b); }
inline V Sub(V a, V b) { return simde_mm_sub_ps(a, b); }
template <int kImm>
inline V Splat(V v) {  // vspltw: guest lane 0 = imm 0xFF, 1 = 0xAA, 2 = 0x55, 3 = 0x00
  return simde_mm_castsi128_ps(simde_mm_shuffle_epi32(simde_mm_castps_si128(v), kImm));
}
inline V SplatBits(u32 bits) { return simde_mm_castsi128_ps(simde_mm_set1_epi32(int32_t(bits))); }
// stfs f1 (any mode) then lvlx + vspltw 0: the splat of the single's bits.
inline V SplatSingle(double d) { return SplatBits(std::bit_cast<u32>(rex::ppc::DoubleToSingleAnyMode(d))); }

struct Matrix {
  V r0, r1, r2, r3;  // v92..v95
};

// sub_82515E48 / sub_82515E30: the game's sine / cosine table (65536 steps a turn).
constexpr u32 kSinTable = 0x82984C80;
inline double TableSin(const Mem& m, u32 a) { return rex::ppc::SingleToDoubleAnyMode(m.U32(kSinTable + ((a << 2) & 0x3FFFCu))); }
inline double TableCos(const Mem& m, u32 a) {
  return rex::ppc::SingleToDoubleAnyMode(m.U32(kSinTable + (((a + 16384u) << 2) & 0x3FFFCu)));
}

// sub_82513308: identity.
inline void Identity(Matrix& x) {
  const V one = simde_mm_cvtepi32_ps(simde_mm_set1_epi32(1));
  const simde__m128i v12 = simde_mm_castps_si128(one), v13 = simde_mm_set1_epi32(0);
  const simde__m128i v11 = simde_mm_alignr_epi8(v12, v13, 4);
  x.r0 = simde_mm_castsi128_ps(v11);
  x.r1 = simde_mm_castsi128_ps(simde_mm_alignr_epi8(v13, v11, 4));
  x.r2 = simde_mm_castsi128_ps(simde_mm_alignr_epi8(v13, v11, 8));
  x.r3 = simde_mm_castsi128_ps(simde_mm_alignr_epi8(v13, v11, 12));
}
// sub_82513670 (f1, f2, f3 = x, y, z).
inline void Translate(Matrix& x, double f1, double f2, double f3) {
  const V s1 = SplatSingle(f1), s2 = SplatSingle(f2), s3 = SplatSingle(f3);
  V v11 = Mul(x.r0, s1);
  V v0 = Add(Mul(x.r1, s2), v11);
  v0 = Add(Mul(x.r2, s3), v0);
  x.r3 = Add(v0, x.r3);
}
// sub_82513330 (r3 = the 64-byte matrix).
inline void MultiplyBy(const Mem& m, Matrix& x, u32 at) {
  const V m0 = m.LoadA(at), m1 = m.LoadA(at + 16), m2 = m.LoadA(at + 32), m3 = m.LoadA(at + 48);
  auto row = [&](V r) {
    V a = Mul(Splat<0xFF>(r), x.r0);
    a = Add(Mul(Splat<0xAA>(r), x.r1), a);
    a = Add(Mul(Splat<0x55>(r), x.r2), a);
    return Add(Mul(Splat<0x00>(r), x.r3), a);
  };
  const V n0 = row(m0), n2 = row(m2), n1 = row(m1), n3 = row(m3);
  x.r0 = n0;
  x.r1 = n1;
  x.r2 = n2;
  x.r3 = n3;
}
// sub_825134B0 (X), sub_82513530 (Y), sub_825135B0 (Z); r3 = the angle.
inline void RotateX(const Mem& m, Matrix& x, u32 a) {
  const V s = SplatSingle(TableSin(m, a)), c = SplatSingle(TableCos(m, a));
  const V v9 = Mul(x.r1, s), v13 = Mul(x.r1, c), v8 = Mul(x.r2, c);
  const V v0 = Add(Mul(s, x.r2), v13);
  x.r2 = Sub(v8, v9);
  x.r1 = v0;
}
inline void RotateY(const Mem& m, Matrix& x, u32 a) {
  const V s = SplatSingle(TableSin(m, a)), c = SplatSingle(TableCos(m, a));
  const V v9 = Mul(x.r2, s), v13 = Mul(x.r2, c), v8 = Mul(x.r0, c);
  const V v0 = Add(Mul(s, x.r0), v13);
  x.r0 = Sub(v8, v9);
  x.r2 = v0;
}
inline void RotateZ(const Mem& m, Matrix& x, u32 a) {
  const V s = SplatSingle(TableSin(m, a)), c = SplatSingle(TableCos(m, a));
  const V v9 = Mul(x.r0, s), v13 = Mul(x.r0, c), v8 = Mul(x.r1, c);
  const V v0 = Add(Mul(s, x.r1), v13);
  x.r1 = Sub(v8, v9);
  x.r0 = v0;
}
// The rotations of a particle (its record +148, bits 8-10, then bits 6-7), angles at +124 (X), +128 (Y), +132 (Z):
// the switch of both functions.
inline void Rotations(const Mem& m, Matrix& x, u32 rec) {
  const u32 sel = m.U32(rec + 148) & 0xE00000u;
  auto X = [&] { RotateX(m, x, m.U32(rec + 124)); };
  auto Y = [&] { RotateY(m, x, m.U32(rec + 128)); };
  auto Z = [&] { RotateZ(m, x, m.U32(rec + 132)); };
  if (sel > 0x600000u) {
    if (sel == 0x800000u) {
      X(); Z(); Y();
    } else if (sel == 0xA00000u) {
      Z(); X(); Y();
    } else {
      Y(); X(); Z();
    }
  } else if (sel == 0x600000u) {
    Z(); Y(); X();
  } else if (sel == 0x200000u) {
    Y(); Z(); X();
  } else if (sel == 0x400000u) {
    X(); Y(); Z();
  } else {
    Y(); X(); Z();
  }
  const u32 extra = m.U32(rec + 148) & 0x3000000u;
  if (extra == 0x1000000u) {
    RotateY(m, x, 16384);
  } else if (extra == 0x2000000u) {
    RotateX(m, x, 49152);
  }
}
// sub_82513630 (f3 = 1.0 at both call sites).
inline void Scale(Matrix& x, double f1, double f2, double f3) {
  const V s1 = SplatSingle(f1), s2 = SplatSingle(f2), s3 = SplatSingle(f3);
  x.r0 = Mul(x.r0, s1);
  x.r1 = Mul(x.r1, s2);
  x.r2 = Mul(x.r2, s3);
}
// sub_82515528: (x, y, z, 1) at r4 through the matrix (transposed dot products); the three results' bits.
inline void TransformDot(const Mem& m, const Matrix& x, u32 p, V out[3]) {
  const simde__m128i v12 = simde_mm_castps_si128(x.r0), v11 = simde_mm_castps_si128(x.r1),
                     v10 = simde_mm_castps_si128(x.r2), v8 = simde_mm_castps_si128(x.r3);
  const V v9 = simde_mm_cvtepi32_ps(simde_mm_set1_epi32(1));
  V v6 = m.LoadU(p);
  const simde__m128i v5 = simde_mm_unpackhi_epi32(v10, v12);
  const simde__m128i v4 = simde_mm_unpackhi_epi32(v8, v11);
  const simde__m128i v3 = simde_mm_unpacklo_epi32(v8, v11);
  const simde__m128i v2 = simde_mm_unpacklo_epi32(v10, v12);
  const V v1 = simde_mm_castsi128_ps(simde_mm_unpackhi_epi32(v4, v5));
  const V v31 = simde_mm_castsi128_ps(simde_mm_unpacklo_epi32(v4, v5));
  const V v30 = simde_mm_castsi128_ps(simde_mm_unpackhi_epi32(v3, v2));
  v6 = simde_mm_blend_ps(v6, simde_mm_permute_ps(v9, 228), 1);
  out[0] = simde_mm_dp_ps(v1, v6, 0xFF);
  out[1] = simde_mm_dp_ps(v31, v6, 0xFF);
  out[2] = simde_mm_dp_ps(v30, v6, 0xFF);
}
// sub_825154E8: v1 = r0*x + r3, + r1*y, + r2*z.
inline V TransformMadd(const Mem& m, const Matrix& x, u32 p) {
  const V v7 = m.LoadU(p);
  V v0 = Add(Mul(x.r0, Splat<0xFF>(v7)), x.r3);
  v0 = Add(Mul(x.r1, Splat<0xAA>(v7)), v0);
  return Add(Mul(x.r2, Splat<0x55>(v7)), v0);
}

inline Matrix CtxMatrix(const PPCContext& ctx) {
  return Matrix{simde_mm_load_ps(ctx.v92.f32), simde_mm_load_ps(ctx.v93.f32), simde_mm_load_ps(ctx.v94.f32),
                simde_mm_load_ps(ctx.v95.f32)};
}
// sub_825187D0 / sub_82518820 (through sub_82513BB8 / sub_82524B40): the thread's matrix stack (block = *(r13),
// +28 the stack pointer, +36 a counter). The pop reloads what the push stored, so v92..v95 come back unchanged.
inline void PushMatrix(const Mem& m, u32 r13, const Matrix& x) {
  const u32 blk = m.U32(r13);
  const u32 p = m.U32(blk + 28);
  m.StoreA(p, x.r0);
  m.StoreA(p + 16, x.r1);
  m.StoreA(p + 32, x.r2);
  m.StoreA(p + 48, x.r3);
  m.SetU32(blk + 28, m.U32(blk + 28) + 64);
  m.SetU32(blk + 36, m.U32(blk + 36) - 1);
}
inline void PopMatrix(const Mem& m, u32 r13, Matrix& x) {
  const u32 blk = m.U32(r13);
  m.SetU32(blk + 36, m.U32(blk + 36) + 1);
  const u32 p = m.U32(blk + 28) - 64;
  m.SetU32(blk + 28, p);
  x.r1 = m.LoadA(p + 16);
  x.r2 = m.LoadA(p + 32);
  x.r0 = m.LoadA(p);
  x.r3 = m.LoadA(p + 48);
}

// ---------------------------------------------------------------------------------------------------------------
// Scalar helpers.

// The 3-lane vector add / subtract of sub_82515418 / sub_82515460 (vaddfp / vsubfp, flush mode on). With no
// denormal input or result the flush mode cannot change an add or a subtract (an exact result below the normal
// range is itself representable, i.e. denormal, and caught here), so the scalar unit does it in its own mode; any
// denormal redoes the three lanes in the vector mode as the original, then goes back.
inline bool Denormal(float f) {
  const u32 b = std::bit_cast<u32>(f) & 0x7FFFFFFFu;
  return b - 1u < 0x007FFFFFu;
}
#if defined(__aarch64__)
#define TP_BARRIER(x) __asm__ __volatile__("" : "+w"(x))
#else
#define TP_BARRIER(x) __asm__ __volatile__("" : "+x"(x))
#endif
// The flush-mode switches with compiler barriers: guest memory accesses stay on their side (the float conversions of
// loads included); register values that must be computed in one mode go through TP_BARRIER.
inline void ModeOn(PPCContext& ctx) {
  __asm__ __volatile__("" ::: "memory");
  ctx.fpscr.enableFlushMode();
  __asm__ __volatile__("" ::: "memory");
}
inline void ModeOff(PPCContext& ctx) {
  __asm__ __volatile__("" ::: "memory");
  ctx.fpscr.disableFlushMode();
  __asm__ __volatile__("" ::: "memory");
}
template <bool kSub>
[[gnu::noinline, gnu::cold]] void Vec3Slow(PPCContext& ctx, const float a[3], const float b[3], float r[3]) {
  V va = simde_mm_set_ps(0.0f, a[0], a[1], a[2]), vb = simde_mm_set_ps(0.0f, b[0], b[1], b[2]);
  ModeOn(ctx);
  TP_BARRIER(va);
  TP_BARRIER(vb);
  V vr = kSub ? simde_mm_sub_ps(va, vb) : simde_mm_add_ps(va, vb);
  TP_BARRIER(vr);
  ModeOff(ctx);
  r[0] = std::bit_cast<float>(Bits(vr, 2));
  r[1] = std::bit_cast<float>(Bits(vr, 1));
  r[2] = std::bit_cast<float>(Bits(vr, 0));
}
template <bool kSub>
inline void Vec3(PPCContext& ctx, const float a[3], const float b[3], float r[3]) {
  float t[3];
  bool slow = false;
  for (int i = 0; i < 3; ++i) {
    t[i] = kSub ? a[i] - b[i] : a[i] + b[i];
    slow |= Denormal(a[i]) | Denormal(b[i]) | Denormal(t[i]);
  }
  if (slow) [[unlikely]] {
    Vec3Slow<kSub>(ctx, a, b, t);
  }
  for (int i = 0; i < 3; ++i) r[i] = t[i];
}
inline void Load3(const Mem& m, u32 a, float v[3]) {
  for (int i = 0; i < 3; ++i) v[i] = m.F32(a + 4 * i);
}
inline void Store3(const Mem& m, u32 a, const float v[3]) {  // stvewx: ea & ~3
  for (int i = 0; i < 3; ++i) m.SetU32((a + 4 * i) & ~3u, std::bit_cast<u32>(v[i]));
}
// sub_82515728 / sub_82518738: out[k] = float(double(in[k]) * f1) (flush mode off).
inline void ScaleVec(const float* in, double f1, float* out, int n) {
  for (int i = 0; i < n; ++i) out[i] = float(double(float(double(in[i]) * f1)));
}

// sub_826A1000: the CRT's atan2(f1, f2), transliterated (it only reads its constants and its arguments).
struct Atan2K {
  double z, d8, d16, d24, d40, f176, d56, d64, d72, d80, d88, d96, d104, d112, d128[4];
};
Atan2K LoadAtan2K(const Mem& m) {
  constexpr u32 k = 0x8280E2B8;
  auto d = [&](u32 o) { return std::bit_cast<double>(m.U64(k + o)); };
  Atan2K c{};
  c.z = m.D(k + 168);
  c.d8 = d(8), c.d16 = d(16), c.d24 = d(24), c.d40 = d(40), c.f176 = m.D(k + 176);
  c.d56 = d(56), c.d64 = d(64), c.d72 = d(72), c.d80 = d(80), c.d88 = d(88), c.d96 = d(96), c.d104 = d(104);
  c.d112 = d(112);
  for (int i = 0; i < 4; ++i) c.d128[i] = d(128 + 8 * i);
  return c;
}
inline bool SignBit(double d) { return (std::bit_cast<u64>(d) >> 63) != 0; }
double Atan2(const Atan2K& k, double f1, double f2) {
  double f0;
  if (f2 == k.z) {
    if (!(f1 == k.z)) {
      f0 = k.d8;
    } else {
      if (!SignBit(f2)) return f1;
      return SignBit(f1) ? -k.d16 : k.d16;
    }
  } else {
    double f13 = __builtin_fabs(f2);
    int r10 = 0;
    f0 = __builtin_fabs(f1);
    double f12 = f13;
    if (f0 > f13) {
      f12 = f0;
      r10 = 2;
      f0 = f13;
    }
    f0 = f0 / f12;
    if (f0 > k.d24) {
      const double c = k.d40;
      r10 = r10 + 1;
      const double f11 = c + f0;
      f0 = std::fma(c, f0, -k.f176);
      f0 = f0 / f11;
    }
    const double f5 = f0 * f0;
    double a = std::fma(k.d80, f5, k.d72);
    double b = k.d112 + f5;
    a = std::fma(a, f5, k.d64);
    b = std::fma(b, f5, k.d104);
    a = std::fma(a, f5, k.d56);
    b = std::fma(b, f5, k.d96);
    a = a * f5;
    b = std::fma(b, f5, k.d88);
    a = a * f0;
    a = a / b;
    f0 = a + f0;
    if (r10 > 1) f0 = -f0;
    f0 = k.d128[r10] + f0;
  }
  const double alt = k.d16 - f0;
  double r = rex::ppc::GeZeroAnyMode(f2) ? f0 : alt;
  if (SignBit(f1)) r = -r;
  return r;
}
// sub_82515F30: the angle (65536 a turn) of (f1, f2), the low word of fctiwz.
inline u32 Angle(const Atan2K& k, double f1, double f2) {
  const double f29 = double(float(Atan2(k, f1, f2)));  // it calls atan2 twice with the same arguments
  const double f11 = f29;
  const double f10 = double(float(f29 * RO(0x47800000)));
  const double f9 = rex::ppc::GeZeroAnyMode(f11) ? RO(0x3F000000) : RO(0xBF000000);
  const double f8 = double(float(std::fma(f10, RO(0x3E22F983), f9)));
  return u32(u64(rex::ppc::fctiwz(f8)));
}

// The vertex-slot counters: an atomic add of 1 as the guest's lwarx / stwcx. loop, or (the check's replay) the values
// the original took.
struct Slots {
  bool replay = false;
  u32 next_a = 0, next_b = 0;
  u32 taken_a = 0, taken_b = 0;
};
inline u32 AtomicAdd1(const Mem& m, u32 a) {
  uint32_t* p = reinterpret_cast<uint32_t*>(m.At(a));
  for (;;) {
    uint32_t seen = __atomic_load_n(p, __ATOMIC_RELAXED);
    const u32 old = __builtin_bswap32(seen);
    m.Card(a);
    if (__atomic_compare_exchange_n(p, &seen, __builtin_bswap32(old + 1), false, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST))
      return old;
  }
}
inline u32 Take(const Mem& m, Slots& s, u32 counter, bool first) {
  if (first) {
    ++s.taken_a;
    return s.replay ? s.next_a++ : AtomicAdd1(m, counter);
  }
  ++s.taken_b;
  return s.replay ? s.next_b++ : AtomicAdd1(m, counter);
}

// A generated callee with the registers the original sets (r1 = this frame, lr = the return address).
inline void CallGuest(PPCContext& ctx, uint8_t* base, PPCFunc* f, u32 frame, u32 lr) {
  ctx.r1.u64 = frame;
  ctx.lr = lr;
  f(ctx, base);
}

constexpr u32 kFlag = 0x82935C1C;     // both: 1 = paused, the 10F0 also stops at any non-zero
constexpr u32 kTable = 0x82931288;    // sub_822E05C0: slot table
constexpr u32 kTableCount = 0x82931284;
constexpr u32 kSlots05 = 0x82934178;  // sub_822E05C0: vertex slots (3000)
constexpr u32 kSlots10 = 0x82935CA0;  // sub_822E10F0: vertex slots (2048)

struct Stats {
  std::atomic<u64> calls{0}, particles{0};
};
Stats g_stats05, g_stats10;

// ---------------------------------------------------------------------------------------------------------------
// sub_822E05C0. r3 = the emitter E: +64 a force vector, +80 a target, +104.., +108 records A (160 bytes),
// +112 records B (144 bytes), +116 count, +120 vertices, +128.. parameters, +148 / +152 settings.
void Native05(PPCContext& __restrict ctx, uint8_t* base, Slots& slots) {
  const Mem m{base};
  const u32 E = ctx.r3.u32;
  const u32 sp = ctx.r1.u32;
  const u32 F = sp - 448;
  {  // stwu's back chain (no store card in the generated code either)
    const u32 w = __builtin_bswap32(sp);
    std::memcpy(m.At(F), &w, 4);
  }
  if (m.U32(kFlag) == 1) return;
  static const Atan2K k = LoadAtan2K(m);
  const u32 r7 = m.U32(E + 136), r6 = m.U32(E + 140), r5 = m.U32(E + 148), count0 = m.U32(E + 116);
  ModeOff(ctx);
  const double f0c = RO(0x3E8E38E4);
  u32 r31 = m.U32(E + 108);
  u32 r29 = m.U32(E + 112);
  const double f28 = RO(0x3F800000);
  const double f30 = 0.0;
  const double f12i = FMULS(m.D(r5 + 628), f0c);
  const float lift[3] = {0.0f, float(f12i), 0.0f};  // the stack vector at 112
  const double f7 = double(float(double(int64_t(u64(r7)))));
  const double f6 = double(float(double(int64_t(u64(r6)))));
  const double f20 = FDIVS(f28, f7);
  const double f21 = FDIVS(f28, f6);
  if (count0 == 0) return;  // (r1 restored below the same way: never changed in ctx)
  const double f24 = RO(0x3FC00000), f25 = RO(0x41F00000), f31 = RO(0x3E4CCCCD), f26 = RO(0x40000000),
               f29 = RO(0x3CF5C28F), f27 = RO(0x3A83126F), f22 = RO(0xBC888889), f23 = RO(0x41855555);
  constexpr u32 kCorners = 0x82743460, kUv = 0x8274348C;
  const u32 r13 = ctx.r13.u32;
  u64 done = 0;
  static const u32 ahead = u32(REXCVAR_GET(tekken_native_particles_prefetch));
  for (u32 r20 = 0;;) {
    ++done;
    if (ahead) {  // tekken_native_particles_prefetch: records B (144 bytes) and A (160 bytes) further on
      const uint8_t* b = m.At(r29 + 144 * ahead);
      const uint8_t* a = m.At(r31 + 160 * ahead);
      __builtin_prefetch(b, 1, 0);
      __builtin_prefetch(b + 64, 1, 0);
      __builtin_prefetch(b + 128, 1, 0);
      __builtin_prefetch(a, 1, 0);
      __builtin_prefetch(a + 64, 1, 0);
      __builtin_prefetch(a + 128, 1, 0);
    }
    // Life counter: at zero the particle respawns.
    {
      const int32_t life = int32_t(m.U32(r29 + 88));
      if (life > 0) {
        const u32 left = u32(life - 1);
        m.SetU32(r29 + 88, left);
        if (left == 0) {
          ctx.r5.u64 = r29;
          ctx.r7.u64 = m.U32(E + 152);
          ctx.r4.u64 = r31;
          ctx.r6.u64 = m.U32(E + 148);
          ctx.r3.u64 = E;
          CallGuest(ctx, base, __imp__sub_822E00C0, F, 0x822E0704);
        }
      }
    }
    {
      const u32 a84 = m.U32(r29 + 84);
      m.SetU32(r29 + 108, 0);
      const u32 a104 = m.U32(r29 + 104);
      m.SetU32(r29 + 84, a84 + 1);
      m.SetU32(r29 + 104, a104 + 1);
    }
    float force[4];
    if (m.U32(m.U32(E + 152) + 204) != 0) {
      ModeOff(ctx);  // REX_FLUSH_KNOWN_OFF
      ctx.r3.u64 = F + 144;
      ctx.f3.f64 = m.D(r31 + 8);
      ctx.f2.f64 = m.D(r31 + 4);
      ctx.f1.f64 = m.D(r31 + 0);
      CallGuest(ctx, base, __imp__sub_822B71B8, F, 0x822E0744);
      ModeOff(ctx);
      const double f1 = double(float(m.D(m.U32(E + 148) + 636) * f23));
      float in[4];
      for (int i = 0; i < 4; ++i) in[i] = m.F32(F + 144 + 4 * i);
      ScaleVec(in, f1, force, 4);
    } else {
      for (int i = 0; i < 4; ++i) force[i] = m.F32(E + 64 + 4 * i);
    }
    float p[3], d96[3];
    Load3(m, r29, p);
    Vec3<true>(ctx, p, force, d96);  // sub_82515460 (F+96 = B.pos - force)
    ModeOff(ctx);
    {
      const double f12 = FMULS(m.D(r29 + 12), m.D(m.U32(E + 148) + 624));
      const double f1 = double(float(f12 * f22));
      ScaleVec(d96, f1, d96, 3);  // sub_82515728
    }
    Vec3<true>(ctx, d96, lift, d96);
    ModeOff(ctx);
    float d128[3];
    {
      const double f1 = m.D(E + 128);
      float b[3];
      Load3(m, r29, b);
      ScaleVec(b, f1, d128, 3);
      float a[3], s[3];
      Load3(m, r31, a);
      Vec3<false>(ctx, a, d128, s);  // sub_82515418: A.pos += F+128
      Store3(m, r31, s);
    }
    bool add96 = false;
    if (int32_t(m.U32(r29 + 88)) < 0) {
      ModeOff(ctx);
      const double f0 = m.D(E + 132);
      const double f13 = m.D(r31 + 4);
      if (!(f13 < f0)) {
        add96 = true;
      } else {
        m.SetF(r31 + 4, f0);
        m.SetU32(r29 + 108, 1);
        const u32 idx = Take(m, slots, kTableCount, true);
        m.SetU32(kTable + (idx << 2), m.U32(E + 104) + r20);
        m.SetU32(r29 + 88, m.U32(E + 136));
      }
    } else {
      if (m.U32(r29 + 108) == 1) {
        ModeOff(ctx);
        const double f0 = m.D(r29 + 0), f13 = m.D(r29 + 8);
        const double f12 = double(float(f0 * f27)), f11 = double(float(f13 * f27));
        m.SetF(r29 + 0, f12);
        m.SetF(r29 + 4, f30);
        m.SetF(r29 + 8, f11);
      }
      m.SetU32(r29 + 108, 0);
      ModeOff(ctx);
      const double f0 = m.D(E + 132), f13 = m.D(r31 + 4);
      if (f13 > f0) {
        add96 = true;
      } else {
        m.SetF(r29 + 76, f28);
        m.SetU32(r31 + 4, Quiet(m.U32(E + 132)));
        const double f12 = m.D(r29 + 0), f13b = m.D(r29 + 8);
        const double f10 = double(float(f13b * f29)), f11 = double(float(f12 * f29));
        m.SetF(r29 + 0, f11);
        m.SetF(r29 + 4, f30);
        m.SetF(r29 + 8, f10);
      }
    }
    if (add96) {
      float a[3], s[3];
      Load3(m, r29, a);
      Vec3<false>(ctx, a, d96, s);  // sub_82515418: B.pos += F+96
      Store3(m, r29, s);
    }
    ModeOff(ctx);
    {
      const double f0 = m.D(r29 + 76), f13 = m.D(E + 144), f12 = m.D(m.U32(E + 148) + 540);
      const double f11 = FMULS(f12, f0);
      const double f10 = FMULS(f11, f13);
      m.SetF(r31 + 44, f10);
    }
    double g0;
    {
      const double f9 = m.D(E + 128);
      const double f8 = double(float(f26 - f9));
      g0 = FMULS(f8, f9);
      if (g0 < f31) g0 = f31;
    }
    if (int32_t(m.U32(r29 + 88)) < 0) {
      ModeOff(ctx);  // REX_FLUSH_KNOWN_OFF
      {
        const double f11 = FMULS(m.D(r29 + 68), m.D(r29 + 64));
        m.SetF(r31 + 116, f11);
        const double f10 = m.D(r29 + 64), f9 = m.D(r29 + 72);
        const double f8 = FMULS(f9, f10);
        const double f7b = double(float(f8 * g0));
        m.SetF(r31 + 120, f7b);
      }
      {
        const double f1 = m.D(r29 + 8);
        const double f2 = -m.D(r29 + 4);
        u32 r3 = Angle(k, f1, f2);
        r3 = u32(-u64(r3));
        m.SetU32(r31 + 124, r3);
      }
      {
        const double c = TableCos(m, m.U32(r31 + 124));
        const double f5 = m.D(r29 + 4);
        const double f19 = double(float(c * f5));
        const double s = TableSin(m, m.U32(r31 + 124));
        const double f4 = m.D(r29 + 8);
        const double f3 = double(float(-std::fma(s, f4, -f19)));
        const double f1 = m.D(r29 + 0);
        m.SetU32(r31 + 132, Angle(k, f1, -f3));
      }
      {
        const double f1e = m.D(E + 84), f11 = m.D(E + 88);
        const double f0 = m.D(r31 + 0), f13 = m.D(r31 + 4), f12 = m.D(r31 + 8), f2 = m.D(E + 80);
        const double f17 = FSUBS(f2, f0);
        double f19 = FSUBS(f11, f12);
        double f18 = FSUBS(f1e, f13);
        const double c1 = TableCos(m, m.U32(r31 + 124));
        double f16 = double(float(c1 * f18));
        const double s1 = TableSin(m, m.U32(r31 + 124));
        f16 = double(float(-std::fma(s1, f19, -f16)));
        const double c2 = TableCos(m, m.U32(r31 + 124));
        f19 = double(float(c2 * f19));
        const double s2 = TableSin(m, m.U32(r31 + 124));
        f19 = double(float(std::fma(s2, f18, f19)));
        const double c3 = TableCos(m, m.U32(r31 + 132));
        f18 = double(float(c3 * f17));
        const double s3 = TableSin(m, m.U32(r31 + 132));
        const double f1 = double(float(-std::fma(s3, f16, -f18)));
        m.SetU32(r31 + 128, Angle(k, f1, f19));
      }
    } else {
      ModeOff(ctx);  // REX_FLUSH_KNOWN_OFF
      const double f13 = double(float(m.D(r29 + 64) * f25));
      m.SetF(r31 + 116, f13);
      const double f11 = double(float(m.D(r31 + 44) * f24));
      const double f10 = m.D(r29 + 64);
      m.SetU32(r31 + 124, 16384);
      const double f9 = double(float(f10 * f25));
      m.SetF(r31 + 120, f9);
      m.SetU32(r31 + 128, 0);
      const u32 r11 = m.U32(r29 + 116);
      m.SetF(r31 + 44, f11);
      m.SetU32(r31 + 132, r11 << 4);
    }
    {
      const int32_t life = int32_t(m.U32(r29 + 88));
      ModeOff(ctx);  // REX_FLUSH_KNOWN_OFF (both branches)
      if (life > 0) {
        const double f0 = m.D(r31 + 44);
        const double f11 = double(float(double(int64_t(life))));
        const double f10 = FMULS(f0, f11);
        m.SetF(r31 + 44, double(float(f10 * f20)));
      } else {
        const u32 age = m.U32(r29 + 84);
        if (age < m.U32(E + 140)) {
          const double f0 = m.D(r31 + 44);
          const double f11 = double(float(double(int64_t(int32_t(age)))));
          const double f10 = FMULS(f0, f11);
          m.SetF(r31 + 44, double(float(f10 * f21)));
        }
      }
    }
    {
      const u32 q = Quiet(m.U32(r31 + 44));
      m.SetU32(r31 + 76, q);
      m.SetU32(r31 + 92, q);
      m.SetU32(r31 + 60, q);
      m.SetU32(r31 + 108, q);
    }
    // The quad: matrix on the guest's matrix stack, a vertex slot, the matrix, four corners.
    PushMatrix(m, r13, CtxMatrix(ctx));  // v92..v95 as they are (a guest callee could have changed them)
    const u32 slot = Take(m, slots, kSlots05, false);
    // The scalar inputs of the matrix, read in the scalar mode (nothing writes them in between).
    double tx = m.D(r31 + 0), ty = m.D(r31 + 4), tz = m.D(r31 + 8);
    double sx = m.D(r31 + 116), sy = m.D(r31 + 120);
    TP_BARRIER(tx); TP_BARRIER(ty); TP_BARRIER(tz); TP_BARRIER(sx); TP_BARRIER(sy);
    ModeOn(ctx);
    TP_BARRIER(tx); TP_BARRIER(ty); TP_BARRIER(tz); TP_BARRIER(sx); TP_BARRIER(sy);
    Matrix x;
    Identity(x);
    Translate(x, tx, ty, tz);
    MultiplyBy(m, x, E);
    Rotations(m, x, r31);
    Scale(x, sx, sy, f28);
    V pos[4][3];
    for (int c = 0; c < 4; ++c) {
      TransformDot(m, x, kCorners + 12 * c, pos[c]);
      TP_BARRIER(pos[c][0]); TP_BARRIER(pos[c][1]); TP_BARRIER(pos[c][2]);
    }
    ModeOff(ctx);
    const u32 vbase = slot * 192 + m.U32(E + 120);
    for (int c = 0; c < 4; ++c) {
      const u32 v = vbase + 48 * c;
      const u32 col = r31 + 44 + 16 * c;
      m.SetU32((v + 0) & ~3u, Bits(pos[c][0], 3));
      m.SetU32((v + 4) & ~3u, Bits(pos[c][1], 3));
      m.SetU32((v + 8) & ~3u, Bits(pos[c][2], 3));
      m.SetF(v + 16, FMULS(m.D(r31 + 32), m.D(col + 4)));
      m.SetF(v + 20, FMULS(m.D(r31 + 36), m.D(col + 8)));
      m.SetF(v + 24, FMULS(m.D(r31 + 40), m.D(col + 12)));
      m.SetF(v + 28, FMULS(m.D(r31 + 44), m.D(col + 16)));
      m.SetU32(v + 32, Quiet(m.U32(kUv + 8 * c + 4)));
      m.SetF(v + 36, double(m.F32(kUv + 8 * c + 8)));
    }
    Matrix back;
    PopMatrix(m, r13, back);
    if (slot >= 3000) break;
    ++r20;
    r31 += 160;
    r29 += 144;
    if (!(r20 < m.U32(E + 116))) break;
  }
  ctx.r1.u64 = sp;
  g_stats05.particles.fetch_add(done, std::memory_order_relaxed);
}

// ---------------------------------------------------------------------------------------------------------------
// sub_822E10F0. r3 = the emitter E: +64 a force vector, +80 a centre, +104 records A (160 bytes), +108 records B
// (112 bytes), +112 count, +116 vertices, +124.. parameters, +152 / +156 settings. Returns r3 = 0.
void Native10(PPCContext& __restrict ctx, uint8_t* base, Slots& slots) {
  const Mem m{base};
  const u32 E = ctx.r3.u32;
  const u32 sp = ctx.r1.u32;
  const u32 F = sp - 432;
  {
    const u32 w = __builtin_bswap32(sp);
    std::memcpy(m.At(F), &w, 4);
  }
  u32 flag = m.U32(kFlag);
  if (flag == 1) {
    ctx.r3.u64 = 0;
    return;
  }
  const u32 r11i = m.U32(E + 136);
  ModeOff(ctx);
  const double f12a = FSUBS(m.D(E + 140), m.D(E + 144));
  const u32 r6 = m.U32(E + 152);
  u32 r31 = m.U32(E + 104);
  const double f31 = 0.0;
  u32 r26 = m.U32(E + 108);
  const double f10i = FMULS(m.D(r6 + 628), RO(0x3E8E38E4));
  const double f28 = RO(0x3F800000);
  const float lift[3] = {0.0f, float(f10i), 0.0f};  // the stack vector at 120
  const double f24 = FDIVS(f28, f12a);
  const double f7 = double(float(double(int64_t(int32_t(r11i)))));
  const double f23 = FDIVS(f28, f7);
  if (flag != 0) {
    ctx.r3.u64 = 0;
    return;
  }
  const double f25 = RO(0x3D4CCCCD), f29 = RO(0x3CF5C28F), f30 = RO(0x3A83126F), f26 = RO(0xBC888889),
               f27 = RO(0x41855555);
  constexpr u32 kCorners = 0x82743268, kUv = 0x82743294;
  const u32 r13 = ctx.r13.u32;
  u64 done = 0;
  static const u32 ahead = u32(REXCVAR_GET(tekken_native_particles_prefetch));
  for (u32 r18 = 0;; ++r18, r31 += 160, r26 += 112) {
    if (r18 != 0 && flag != 0) break;  // loc_822E1738's test (the first pass comes from the entry test)
    if (!(r18 < m.U32(E + 112))) break;
    if (ahead) {  // tekken_native_particles_prefetch: records A (160 bytes) and B (112 bytes) further on
      const uint8_t* a = m.At(r31 + 160 * ahead);
      const uint8_t* b = m.At(r26 + 112 * ahead);
      __builtin_prefetch(a, 1, 0);
      __builtin_prefetch(a + 64, 1, 0);
      __builtin_prefetch(a + 128, 1, 0);
      __builtin_prefetch(b, 1, 0);
      __builtin_prefetch(b + 64, 1, 0);
    }
    if (m.U32(r31 + 112) == 0) continue;
    {
      const int32_t life = int32_t(m.U32(r26 + 68));
      if (life > 0) {
        const u32 left = u32(life - 1);
        m.SetU32(r26 + 68, left);
        if (left == 0) {
          ModeOff(ctx);  // REX_FLUSH_KNOWN_OFF
          m.SetF(r31 + 44, f31);
          m.SetU32(r31 + 112, 0);
          m.SetF(r31 + 76, f31);
          m.SetU32(r31 + 148, 0);
          m.SetF(r31 + 92, f31);
          m.SetF(r31 + 60, f31);
          m.SetF(r31 + 108, f31);
          continue;
        }
      }
    }
    ++done;
    m.SetU32(r26 + 64, m.U32(r26 + 64) + 1);
    float force[4];
    if (m.U32(m.U32(E + 156) + 204) != 0) {
      ModeOff(ctx);  // REX_FLUSH_KNOWN_OFF
      ctx.r3.u64 = F + 176;
      ctx.f3.f64 = m.D(r31 + 8);
      ctx.f2.f64 = m.D(r31 + 4);
      ctx.f1.f64 = m.D(r31 + 0);
      CallGuest(ctx, base, __imp__sub_822B71B8, F, 0x822E1268);
      ModeOff(ctx);
      const double f1 = double(float(m.D(m.U32(E + 152) + 636) * f27));
      float in[4];
      for (int i = 0; i < 4; ++i) in[i] = m.F32(F + 176 + 4 * i);
      ScaleVec(in, f1, force, 4);
    } else {
      for (int i = 0; i < 4; ++i) force[i] = m.F32(E + 64 + 4 * i);
    }
    float p[3], d104[3];
    Load3(m, r26, p);
    Vec3<true>(ctx, p, force, d104);  // F+104 = B.pos - force
    ModeOff(ctx);
    {
      const double f12 = FMULS(m.D(r26 + 92), m.D(m.U32(E + 152) + 624));
      const double f1 = double(float(f12 * f26));
      ScaleVec(d104, f1, d104, 3);
    }
    Vec3<true>(ctx, d104, lift, d104);
    ModeOff(ctx);
    {
      const double f1 = m.D(E + 124);
      float b[3], d152[3], a[3], s[3];
      Load3(m, r26, b);
      ScaleVec(b, f1, d152, 3);
      Load3(m, r31, a);
      Vec3<false>(ctx, a, d152, s);  // A.pos += F+152
      Store3(m, r31, s);
    }
    m.SetU32(r31 + 132, (m.U32(r26 + 96) << 9) & 0xFE00u);
    {
      const u32 r11 = m.U32(E + 152);
      const int32_t r8 = int32_t(m.U32(r26 + 68));
      ModeOff(ctx);
      double f0 = m.D(r11 + 628);
      int path;  // 0: loc_822E13D8 directly, 1: add F+104 (loc_822E13C8)
      if (r8 < 0) {
        bool to135C = false;
        if (f0 < f31) {
          to135C = true;
        } else {
          f0 = m.D(r31 + 4);
          if (!(f0 < f31)) to135C = true;
        }
        if (!to135C) {
          m.SetF(r31 + 4, f31);
          const double f12 = m.D(r26 + 8), g = m.D(r26 + 0);
          const double f13 = double(float(g * f30)), f11 = double(float(f12 * f30));
          m.SetF(r26 + 0, f13);
          m.SetF(r26 + 4, f31);
          m.SetF(r26 + 8, f11);
          m.SetU32(r26 + 68, m.U32(E + 132));
          path = 0;
        } else {
          const int32_t a76 = int32_t(m.U32(r26 + 76));
          if (a76 > 0 && a76 == int32_t(m.U32(r26 + 64))) {
            float a[3], s[3];
            Load3(m, r26, a);
            Vec3<false>(ctx, a, d104, s);
            Store3(m, r26, s);
            m.SetU32(r26 + 68, m.U32(E + 132));
            path = 0;
          } else {
            path = 1;
          }
        }
      } else {
        ModeOff(ctx);  // REX_FLUSH_KNOWN_OFF
        if (f0 < f31) {
          path = 1;
        } else {
          f0 = m.D(r31 + 4);
          if (f0 > f31) {
            path = 1;
          } else {
            m.SetF(r31 + 4, f31);
            m.SetF(r26 + 4, f31);
            const double g = m.D(r26 + 8), f12 = m.D(r26 + 0);
            const double f13 = double(float(g * f29)), f11 = double(float(f12 * f29));
            m.SetF(r26 + 0, f11);
            m.SetF(r26 + 8, f13);
            path = 0;
          }
        }
      }
      if (path == 1) {
        float a[3], s[3];
        Load3(m, r26, a);
        Vec3<false>(ctx, a, d104, s);
        Store3(m, r26, s);
      }
    }
    {
      float a[3], c[3], d136[3];
      Load3(m, r31, a);
      Load3(m, E + 80, c);
      Vec3<true>(ctx, a, c, d136);  // F+136 = A.pos - centre
      ModeOff(ctx);
      const double z = d136[2];
      const double f11 = FMULS(z, z);
      const double x0 = d136[0];
      const double lim = m.D(E + 140);
      const double y = d136[1];
      const double f10 = double(float(std::fma(y, y, f11)));
      const double f9 = double(float(std::fma(x0, x0, f10)));
      double f0 = FSQRTS(f9);
      if (f0 > lim) f0 = lim;
      ModeOff(ctx);  // REX_FLUSH_KNOWN_OFF
      f0 = FSUBS(f0, m.D(E + 144));
      if (f0 < f31) f0 = f31;
      ModeOff(ctx);  // REX_FLUSH_KNOWN_OFF
      const double f13 = m.D(r26 + 60), f12 = m.D(E + 148), f11b = m.D(m.U32(E + 152) + 540);
      const double f10b = FMULS(f11b, f13);
      const double f9b = FMULS(f10b, f12);
      const double f8 = double(float(f9b * f0));
      const double f7b = double(float(f8 * f24));
      m.SetF(r31 + 44, f7b);
      const double h13 = m.D(r26 + 48), h0 = m.D(r26 + 52);
      m.SetF(r31 + 116, FMULS(h0, h13));
      const double h11 = m.D(r26 + 56), h10 = m.D(r26 + 48);
      m.SetF(r31 + 120, FMULS(h11, h10));
      const int32_t life = int32_t(m.U32(r26 + 68));
      if (life > 0) {
        const double g11 = double(float(double(int64_t(life))));
        const double g10 = FMULS(f7b, g11);
        ModeOff(ctx);  // REX_FLUSH_KNOWN_OFF
        m.SetF(r31 + 44, double(float(g10 * f25)));
      } else {
        const int32_t age = int32_t(m.U32(r26 + 64));
        if (age < int32_t(m.U32(E + 136))) {
          ModeOff(ctx);  // REX_FLUSH_KNOWN_OFF
          const double g0 = m.D(r31 + 44);
          const double g11 = double(float(double(int64_t(age))));
          const double g10 = FMULS(g0, g11);
          m.SetF(r31 + 44, double(float(g10 * f23)));
        }
      }
    }
    flag = m.U32(kFlag);
    {
      const u32 q = Quiet(m.U32(r31 + 44));
      m.SetU32(r31 + 76, q);
      m.SetU32(r31 + 92, q);
      m.SetU32(r31 + 60, q);
      m.SetU32(r31 + 108, q);
    }
    if (flag != 0) continue;
    if ((m.U32(r31 + 148) & 0x4000000u) != 0 && m.U32(r31 + 112) == 0) continue;
    PushMatrix(m, r13, CtxMatrix(ctx));  // v92..v95 as they are (a guest callee could have changed them)
    const u32 slot = Take(m, slots, kSlots10, false);
    double tx = m.D(r31 + 0), ty = m.D(r31 + 4), tz = m.D(r31 + 8);
    double sx = m.D(r31 + 116), sy = m.D(r31 + 120);
    TP_BARRIER(tx); TP_BARRIER(ty); TP_BARRIER(tz); TP_BARRIER(sx); TP_BARRIER(sy);
    ModeOn(ctx);
    TP_BARRIER(tx); TP_BARRIER(ty); TP_BARRIER(tz); TP_BARRIER(sx); TP_BARRIER(sy);
    Matrix x;
    Identity(x);
    Translate(x, tx, ty, tz);
    MultiplyBy(m, x, E);
    Rotations(m, x, r31);
    Scale(x, sx, sy, f28);
    V pos[4];
    for (int c = 0; c < 4; ++c) {
      pos[c] = TransformMadd(m, x, kCorners + 12 * c);
      TP_BARRIER(pos[c]);
    }
    ModeOff(ctx);
    const u32 vbase = m.U32(E + 116) + slot * 192;
    for (int c = 0; c < 4; ++c) {
      const u32 v = vbase + 48 * c;
      const u32 col = r31 + 44 + 16 * c;
      m.SetU32(v + 0, Quiet(Bits(pos[c], 3)));
      m.SetU32(v + 4, Quiet(Bits(pos[c], 2)));
      m.SetU32(v + 8, Quiet(Bits(pos[c], 1)));
      m.SetF(v + 16, FMULS(m.D(r31 + 32), m.D(col + 4)));
      m.SetF(v + 20, FMULS(m.D(r31 + 36), m.D(col + 8)));
      m.SetF(v + 24, FMULS(m.D(r31 + 40), m.D(col + 12)));
      m.SetF(v + 28, FMULS(m.D(r31 + 44), m.D(col + 16)));
      m.SetU32(v + 32, Quiet(m.U32(kUv + 8 * c + 4)));
      m.SetF(v + 36, double(m.F32(kUv + 8 * c + 8)));
    }
    {
      // stvx v1 to the frame's +192 (with its store card, as the generated code): corner 3's is the one left.
      m.StoreA(F + 192, pos[3]);
    }
    Matrix back;
    PopMatrix(m, r13, back);
    if (slot >= 2048) break;
    flag = m.U32(kFlag);
  }
  ctx.r1.u64 = sp;
  ctx.r3.u64 = 0;
  g_stats10.particles.fetch_add(done, std::memory_order_relaxed);
}

}  // namespace

// ---------------------------------------------------------------------------------------------------------------
// tekken_native_particles_check: original first (live), its outputs saved; inputs restored; the native run with the
// counter values the original took; every byte both write compared; on a difference the original's outputs are put
// back. The two functions' checks run one at a time (a mutex). A call where another thread moved a slot counter
// while the original ran (its slots are then not consecutive) cannot be replayed: counted as skipped, the original's
// result kept.
namespace {

struct Span {
  u32 addr = 0;
  std::vector<uint8_t> bytes;
};
inline Span Save(const Mem& m, u32 a, u32 len) {
  Span s;
  s.addr = a;
  s.bytes.assign(m.At(a), m.At(a) + len);
  return s;
}
inline void Put(const Mem& m, const Span& s) {
  if (!s.bytes.empty()) std::memcpy(m.At(s.addr), s.bytes.data(), s.bytes.size());
}
inline Span Sub(const Span& s, u32 off, u32 len) {
  Span r;
  r.addr = s.addr + off;
  r.bytes.assign(s.bytes.begin() + off, s.bytes.begin() + off + len);
  return r;
}

std::mutex g_check_mutex;
std::atomic<u64> g_check_calls{0}, g_check_particles{0}, g_check_skipped{0}, g_check_differ{0}, g_check_logged{0},
    g_check_race{0}, g_check_race_differ{0};

struct Layout {
  const char* name;
  PPCFunc* original;
  void (*native)(PPCContext&, uint8_t*, Slots&);
  u32 count_off, a_off, a_size, b_off, b_size, vtx_off;
  u32 counter_a;  // the slot table's counter (0: none)
  u32 table;
  u32 counter_b;  // the vertex slots' counter
  bool r3_zero;
  Stats* stats;
};

void Check(PPCContext& __restrict ctx, uint8_t* base, const Layout& L) {
  std::lock_guard<std::mutex> lock(g_check_mutex);
  const Mem m{base};
  const u32 E = ctx.r3.u32;
  const u32 n = m.U32(E + L.count_off);
  if (m.U32(kFlag) == 1 || n == 0 || n > 20000) {
    L.original(ctx, base);
    return;
  }
  constexpr u32 kRng = 0x829C4C90;
  const u32 recs_a = m.U32(E + L.a_off), recs_b = m.U32(E + L.b_off);
  const u32 block = m.U32(ctx.r13.u32);
  const u32 mstack = m.U32(block + 28) & ~0xFu;
  // Inputs.
  const Span hdr0 = Save(m, E, 160), a0 = Save(m, recs_a, n * L.a_size), b0 = Save(m, recs_b, n * L.b_size);
  const u32 rng0 = m.U32(kRng);
  const u32 ca0 = L.counter_a ? m.U32(L.counter_a) : 0, cb0 = m.U32(L.counter_b);
  const u32 vtx0 = m.U32(E + L.vtx_off);
  const Span vpre = Save(m, vtx0 + cb0 * 192, n * 192);
  const Span tpre = L.counter_a ? Save(m, L.table + ca0 * 4, n * 4) : Span{};
  const Span spre = Save(m, mstack, 64);
  const Span bpre = Save(m, block + 28, 12);
  PPCContext* saved = new PPCContext(ctx);
  // The original.
  L.original(ctx, base);
  const u32 ca1 = L.counter_a ? m.U32(L.counter_a) : 0, cb1 = m.U32(L.counter_b);
  const u32 da = ca1 - ca0, db = cb1 - cb0;
  if (da > n || db > n) {  // a counter reset, or moved far by other threads: nothing to replay
    g_check_calls.fetch_add(1, std::memory_order_relaxed);
    g_check_skipped.fetch_add(1, std::memory_order_relaxed);
    delete saved;
    return;
  }
  const Span hdr1 = Save(m, E, 160), a1 = Save(m, recs_a, n * L.a_size), b1 = Save(m, recs_b, n * L.b_size);
  const u32 rng1 = m.U32(kRng);
  const u32 vtx1 = m.U32(E + L.vtx_off);
  const Span v1 = Save(m, vtx0 + cb0 * 192, db * 192);
  const Span t1 = L.counter_a ? Save(m, L.table + ca0 * 4, da * 4) : Span{};
  const Span s1 = Save(m, mstack, 64), bl1 = Save(m, block + 28, 12);
  PPCContext* after = new PPCContext(ctx);
  // Inputs back, then the native with the original's counter values.
  Put(m, hdr0), Put(m, a0), Put(m, b0);
  m.SetU32(kRng, rng0);
  Put(m, Sub(vpre, 0, db * 192));
  if (L.counter_a) Put(m, Sub(tpre, 0, da * 4));
  Put(m, spre), Put(m, bpre);
  ctx = *saved;
  rex::platform::FPSCRPlatform::setcsr(ctx.fpscr.csr);
  Slots slots;
  slots.replay = true;
  slots.next_a = ca0;
  slots.next_b = cb0;
  static const bool control = REXCVAR_GET(tekken_native_particles_check_control);
  if (control) {
    // The control: the original once more instead of the native (its counters set back to the values it started
    // from: test runs only), to measure what the harness itself reports when both sides run the same code.
    if (L.counter_a) m.SetU32(L.counter_a, ca0);
    m.SetU32(L.counter_b, cb0);
    L.original(ctx, base);
    slots.taken_a = L.counter_a ? m.U32(L.counter_a) - ca0 : 0;
    slots.taken_b = m.U32(L.counter_b) - cb0;
  } else {
    L.native(ctx, base, slots);
  }
  const u32 rng_native = m.U32(kRng);
  bool skipped = false;
  const char* what = nullptr;
  u32 where = 0, region = 0;
  uint8_t got8[8] = {}, want8[8] = {};
  if (slots.taken_a != da || slots.taken_b != db) {
    if (slots.taken_a <= da && slots.taken_b <= db) {
      skipped = true;  // another thread's allocation came in between (a native bug would also show as differences)
    } else {
      what = "slot count";
    }
  }
  auto cmp = [&](const char* name, const Span& want) {
    if (what || skipped || want.bytes.empty()) return;
    const uint8_t* got = m.At(want.addr);
    for (size_t i = 0; i < want.bytes.size(); ++i) {
      if (got[i] != want.bytes[i]) {
        what = name;
        where = want.addr + u32(i);
        region = want.addr;
        const size_t o = i & ~size_t(3);
        for (size_t k = 0; k < 8 && o + k < want.bytes.size(); ++k) {
          got8[k] = got[o + k];
          want8[k] = want.bytes[o + k];
        }
        return;
      }
    }
  };
  if (!skipped) {
    cmp("emitter", hdr1);
    cmp("records A", a1);
    cmp("records B", b1);
    cmp("vertices", v1);
    if (L.counter_a) cmp("slot table", t1);
    cmp("matrix stack", s1);
    cmp("matrix stack words", bl1);
    if (!what && m.U32(E + L.vtx_off) != vtx1) what = "vertex base";
    if (!what && ctx.r1.u64 != after->r1.u64) what = "r1";
    if (!what && (ctx.fpscr.csr & rex::platform::FPSCRPlatform::FlushMask) !=
                     (after->fpscr.csr & rex::platform::FPSCRPlatform::FlushMask))
      what = "flush mode";
    if (!what && L.r3_zero && ctx.r3.u64 != after->r3.u64) what = "r3";
    if (!what) {
      const PPCVRegister* g[] = {&ctx.v92, &ctx.v93, &ctx.v94, &ctx.v95, &ctx.v127};
      const PPCVRegister* w[] = {&after->v92, &after->v93, &after->v94, &after->v95, &after->v127};
      for (int i = 0; i < 5; ++i)
        if (std::memcmp(g[i], w[i], 16) != 0) what = "v92..v95 / v127";
    }
  }
  if (skipped || what) {
    // Keep the original's result.
    Put(m, hdr1), Put(m, a1), Put(m, b1), Put(m, v1), Put(m, s1), Put(m, bl1);
    if (L.counter_a) Put(m, t1);
    m.SetU32(kRng, rng1);
    ctx = *after;
    rex::platform::FPSCRPlatform::setcsr(ctx.fpscr.csr);
  }
  // The game's RNG word (0x829C4C90, sub_82518110: no lock) is also stepped by other threads; only the respawn
  // (sub_822E00C0) reads it here. When the native's run ends on another RNG value than the original's, another
  // thread stepped it during one of the two runs (both run the same respawn code from the same value): such a call
  // is counted apart (its records can differ in the respawned particles), not as a difference.
  const bool race = !L.r3_zero && rng_native != rng1;
  const u64 calls = g_check_calls.fetch_add(1, std::memory_order_relaxed) + 1;
  if (race) {
    g_check_race.fetch_add(1, std::memory_order_relaxed);
    if (what) g_check_race_differ.fetch_add(1, std::memory_order_relaxed);
  } else if (skipped) {
    g_check_skipped.fetch_add(1, std::memory_order_relaxed);
  } else {
    g_check_particles.fetch_add(n, std::memory_order_relaxed);
    if (what) {
      g_check_differ.fetch_add(1, std::memory_order_relaxed);
      if (g_check_logged.fetch_add(1, std::memory_order_relaxed) < 40) {
        std::fprintf(stderr,
                     "[tekken_native_particles] DIFFERENCE: %s emitter %08X (%u particles): %s at %08X (+%u in it): "
                     "native %02X%02X%02X%02X %02X%02X%02X%02X, original %02X%02X%02X%02X %02X%02X%02X%02X; rng %s\n",
                     L.name, E, n, what, where, where - region, got8[0], got8[1], got8[2], got8[3], got8[4], got8[5],
                     got8[6], got8[7], want8[0], want8[1], want8[2], want8[3], want8[4], want8[5], want8[6], want8[7],
                     rng_native == rng1 ? (rng0 == rng1 ? "untouched" : "same") : "differs");
        REXLOG_WARN("[tekken_native_particles] DIFFERENCE: {} emitter {:08X} ({} particles): {} at {:08X}", L.name,
                    E, n, what, where);
      }
    }
  }
  if (calls % 500 == 0) {
    const u64 sk = g_check_skipped.load();
    std::fprintf(stderr,
                 "[tekken_native_particles] check: %llu calls compared (%llu particles), %llu skipped, %llu differ; "
                 "%llu with an RNG race (%llu of them differ)\n",
                 (unsigned long long)(calls - sk - g_check_race.load()), (unsigned long long)g_check_particles.load(),
                 (unsigned long long)sk, (unsigned long long)g_check_differ.load(),
                 (unsigned long long)g_check_race.load(), (unsigned long long)g_check_race_differ.load());
    REXLOG_INFO("[tekken_native_particles] check: {} calls compared ({} particles), {} skipped, {} differ; {} with an "
                "RNG race ({} of them differ)",
                calls - sk - g_check_race.load(), g_check_particles.load(), sk, g_check_differ.load(),
                g_check_race.load(), g_check_race_differ.load());
  }
  delete saved;
  delete after;
}

void Native05Run(PPCContext& ctx, uint8_t* base, Slots& s) { Native05(ctx, base, s); }
void Native10Run(PPCContext& ctx, uint8_t* base, Slots& s) { Native10(ctx, base, s); }

const Layout k05 = {"sub_822E05C0", __imp__sub_822E05C0, Native05Run, 116, 108, 160, 112, 144, 120,
                    kTableCount,    kTable,              kSlots05,    false, &g_stats05};
const Layout k10 = {"sub_822E10F0", __imp__sub_822E10F0, Native10Run, 112, 104, 160, 108, 112, 116,
                    0,              0,                   kSlots10,    true,  &g_stats10};

void Dispatch(PPCContext& __restrict ctx, uint8_t* base, const Layout& L) {
  static const bool on = REXCVAR_GET(tekken_native_particles);
  static const bool check = REXCVAR_GET(tekken_native_particles_check);
  const u64 calls = L.stats->calls.fetch_add(1, std::memory_order_relaxed) + 1;
  if ((calls & (calls - 1)) == 0 && calls >= 256) {  // 256, 512, 1024... calls: a sign of life in the log
    REXLOG_INFO("[tekken_native_particles] {}: {} calls, {} particles natively ({})", L.name, calls,
                L.stats->particles.load(), check ? "check" : on ? "native" : "original");
    std::fprintf(stderr, "[tekken_native_particles] %s: %llu calls, %llu particles natively (%s)\n", L.name,
                 (unsigned long long)calls, (unsigned long long)L.stats->particles.load(),
                 check ? "check" : on ? "native" : "original");
  }
  // The native needs the frame 16-byte aligned (the original's lvx / lvlx of its stack temporaries assume it).
  if ((ctx.r1.u32 & 0xF) != 0) [[unlikely]] {
    L.original(ctx, base);
    return;
  }
  if (renut::gpu::render_census::Enabled()) {
    const Mem m{base};
    const u32 emitter = ctx.r3.u32;
    if (m.U32(emitter + L.count_off) && m.U32(kFlag) == 0) {
      const u32 vertices = m.U32(emitter + L.vtx_off);
      if (vertices)
        renut::gpu::render_census::NoteParticlePool(m.At(vertices), L.counter_a != 0);
    }
  }
  if (check) {
    Check(ctx, base, L);
  } else if (on) {
    Slots s;
    L.native(ctx, base, s);
  } else {
    L.original(ctx, base);
  }
}

}  // namespace

#if !defined(TP_NO_HOOKS)
REX_HOOK_RAW(sub_822E05C0) { Dispatch(ctx, base, k05); }
REX_HOOK_RAW(sub_822E10F0) { Dispatch(ctx, base, k10); }
#endif
