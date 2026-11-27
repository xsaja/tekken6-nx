// tekken6-nx (2026-11-19): NOT BUILT (out of app/CMakeLists.txt), kept as the 2D-art trace's tool. To build it again, take its four hooked functions out of app/src/cpu/hot/localized_bulk.txt first.
// tekken6-nx (2026-11-18, diagnostic, off by default): the 2D library's transform writers, logged to stderr.
//
// The missing / garbled 2D art on the Switch: the draw nodes' transform (block+32, read into vs c128..c131) is written
// with a zero scale or left unwritten, while the PC writes it right (docs/switch-port-log.md, "2D transform
// trace"). Two parameter writers store it (they copy a 4x4 matrix into the node's constant block and advance the cursor
// at [r4]):
//   sub_82542AA8: the current matrix (v92..v95, the library's matrix register set) times the 2D camera's (sub_825248E0
//                 on 0x829C9D80), pushed and popped around it (sub_825187D0 / sub_82518820);
//   sub_82542C20: the matrix kept in the source object at r3+32..r3+95.
// tekken_diag_matrix logs each call's inputs and what it wrote, to compare the same sprite on the PC and the Switch.

#include <rex/cvar.h>
#include <rex/hook.h>
#include <rex/logging.h>
#include <rex/ppc.h>

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <mutex>

REXCVAR_DEFINE_BOOL(tekken_diag_matrix, false, "Tekken6/Diagnostics",
                    "Log to stderr ([matrix]) the inputs and outputs of the 2D transform writers sub_82542AA8 / "
                    "sub_82542C20, up to 600 lines (temporary)")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);

REX_EXTERN(__imp__sub_82542AA8);
REX_EXTERN(__imp__sub_82542C20);
REX_EXTERN(__imp__sub_822E8E08);
REX_EXTERN(__imp__sub_8256F458);

namespace {

uint32_t LoadU32(uint8_t* base, uint32_t va) {
  uint32_t v;
  std::memcpy(&v, base + va, 4);
  return __builtin_bswap32(v);
}

float AsFloat(uint32_t bits) {
  float f;
  std::memcpy(&f, &bits, 4);
  return f;
}

void AppendVec(char*& p, char* end, const char* name, const PPCVRegister& v) {
  // The register's words in guest element order (element 0 is the host's last word).
  p += std::snprintf(p, size_t(end - p), " %s(%g %g %g %g)", name, double(v.f32[3]), double(v.f32[2]),
                     double(v.f32[1]), double(v.f32[0]));
}

void AppendMem(char*& p, char* end, const char* name, uint8_t* base, uint32_t va, int words) {
  p += std::snprintf(p, size_t(end - p), " %s %08X(", name, va);
  for (int i = 0; i < words && p < end; ++i)
    p += std::snprintf(p, size_t(end - p), i ? " %g" : "%g", double(AsFloat(LoadU32(base, va + 4 * uint32_t(i)))));
  if (p < end) p += std::snprintf(p, size_t(end - p), ")");
}

bool Take() {
  static std::mutex mutex;
  static int lines = 0;
  std::lock_guard lock(mutex);
  return lines++ < 600;
}

}  // namespace

REX_HOOK_RAW(sub_82542AA8) {
  static const bool on = REXCVAR_GET(tekken_diag_matrix);
  if (!on || !Take()) {
    __imp__sub_82542AA8(ctx, base);
    return;
  }
  const uint32_t src = ctx.r3.u32, cursor_va = ctx.r4.u32, out = LoadU32(base, cursor_va);
  char line[1400];
  char* p = line;
  char* end = line + sizeof(line);
  p += std::snprintf(p, size_t(end - p), "[matrix] AA8 src %08X out %08X r13 %08X lr %08X", src, out, ctx.r13.u32,
                     uint32_t(ctx.lr));
  // The callers: each frame's saved LR sits 8 bytes below the caller's stack pointer (the back chain at [r1]).
  {
    uint32_t frame = ctx.r1.u32;
    for (int i = 0; i < 8 && frame; ++i) {
      const uint32_t previous = LoadU32(base, frame);
      if (!previous || previous <= frame || previous - frame > 0x10000) break;
      p += std::snprintf(p, size_t(end - p), " <- %08X", LoadU32(base, previous - 8));
      frame = previous;
    }
  }
  p += std::snprintf(p, size_t(end - p), " in:");
  AppendVec(p, end, "v92", ctx.v92);
  AppendVec(p, end, "v93", ctx.v93);
  AppendVec(p, end, "v94", ctx.v94);
  AppendVec(p, end, "v95", ctx.v95);
  __imp__sub_82542AA8(ctx, base);
  AppendMem(p, end, "| wrote", base, out, 16);
  std::fprintf(stderr, "%s\n", line);
  std::fflush(stderr);
}

REX_HOOK_RAW(sub_82542C20) {
  static const bool on = REXCVAR_GET(tekken_diag_matrix);
  if (!on || !Take()) {
    __imp__sub_82542C20(ctx, base);
    return;
  }
  const uint32_t src = ctx.r3.u32, cursor_va = ctx.r4.u32, out = LoadU32(base, cursor_va);
  char line[1400];
  char* p = line;
  char* end = line + sizeof(line);
  p += std::snprintf(p, size_t(end - p), "[matrix] C20 src %08X out %08X lr %08X", src, out, uint32_t(ctx.lr));
  AppendMem(p, end, "src+0", base, src, 8);
  AppendMem(p, end, "src+32", base, src + 32, 16);
  __imp__sub_82542C20(ctx, base);
  AppendMem(p, end, "| wrote", base, out, 16);
  std::fprintf(stderr, "%s\n", line);
  std::fflush(stderr);
}

// sub_822E8E08 (an effect item's draw): identity x the item's matrix ([r4+44]) x a fixed scale, then the draw. The
// item, its matrix and its float fields, before the call.
REX_HOOK_RAW(sub_822E8E08) {
  static const bool on = REXCVAR_GET(tekken_diag_matrix);
  if (!on || !Take()) {
    __imp__sub_822E8E08(ctx, base);
    return;
  }
  const uint32_t item = ctx.r4.u32, mat = LoadU32(base, item + 44);
  char line[1400];
  char* p = line;
  char* end = line + sizeof(line);
  p += std::snprintf(p, size_t(end - p), "[matrix] E08 r3 %08X item %08X lr %08X words %08X %08X %08X %08X %08X %08X",
                     ctx.r3.u32, item, uint32_t(ctx.lr), LoadU32(base, item), LoadU32(base, item + 4),
                     LoadU32(base, item + 8), LoadU32(base, item + 28), LoadU32(base, item + 32), LoadU32(base, item + 36));
  AppendMem(p, end, "item+12", base, item + 12, 4);
  AppendMem(p, end, "mat", base, mat, 16);
  std::fprintf(stderr, "%s\n", line);
  std::fflush(stderr);
  __imp__sub_822E8E08(ctx, base);
}

// sub_8256F458: out (r3) = A (r4) x B (r5), 2D affine 4x4 (zeroes out first). The calls whose result has a zero 2x2,
// with both inputs, and a sample of the ones that do not.
REX_HOOK_RAW(sub_8256F458) {
  static const bool on = REXCVAR_GET(tekken_diag_matrix);
  if (!on) {
    __imp__sub_8256F458(ctx, base);
    return;
  }
  const uint32_t out = ctx.r3.u32, a = ctx.r4.u32, b = ctx.r5.u32, lr = uint32_t(ctx.lr);
  char line[3000];
  char* p = line;
  char* end = line + sizeof(line);
  p += std::snprintf(p, size_t(end - p), "[matrix] F458 out %08X a %08X b %08X lr %08X", out, a, b, lr);
  // From sub_8256FAC8 (lr 8256FB30): r31 is the animation object; [obj+32] its file context (sub_8256FEF0): [+0]
  // the file data, [+8] the matrix table (data + [data+84]), [+16] its count ([data+88]).
  if (lr == 0x8256FB30u) {
    const uint32_t obj = ctx.r31.u32, fctx = LoadU32(base, obj + 32);
    const uint32_t data = LoadU32(base, fctx), table = LoadU32(base, fctx + 8), count = LoadU32(base, fctx + 16);
    p += std::snprintf(p, size_t(end - p), " | anim %08X fctx %08X data %08X table %08X count %u hdr", obj, fctx, data,
                       table, count);
    for (int i = 0; i < 40; ++i) p += std::snprintf(p, size_t(end - p), " %08X", LoadU32(base, data + 4 * uint32_t(i)));
    // How much of the table is zero, and the first non-zero word after b.
    uint32_t zeros = 0;
    for (uint32_t i = 0; i < count * 16 && i < 4096; ++i) zeros += LoadU32(base, table + 4 * i) == 0;
    p += std::snprintf(p, size_t(end - p), " | table zero words %u of %u", zeros, count * 16);
    // The node sub_8256D598 got (r29 in sub_8256FAC8), its words, and the r4/r5 sub_8256FAC8 was called with.
    const uint32_t node = ctx.r29.u32;
    p += std::snprintf(p, size_t(end - p), " | node %08X (data+%X):", node, node - data);
    for (int i = 0; i < 8; ++i) p += std::snprintf(p, size_t(end - p), " %08X", LoadU32(base, node + 4 * uint32_t(i)));
    p += std::snprintf(p, size_t(end - p), " r28 %08X r30 %08X", ctx.r28.u32, ctx.r30.u32);
    // The frame-list table (sub_8256FEF0: [fctx+56] = data + [data+124], [data+128] entries of 8 bytes; sub_8256FAC8
    // takes entry [r4+8] and its +4 as the command list's offset): every entry, to see which one r28 came from.
    const uint32_t lists = LoadU32(base, fctx + 56), lcount = LoadU32(base, data + 128);
    p += std::snprintf(p, size_t(end - p), " | lists %08X n %u:", lists, lcount);
    for (uint32_t i = 0; i < lcount + 3 && i < 16; ++i)
      p += std::snprintf(p, size_t(end - p), " [%u]%X/%X", i, LoadU32(base, lists + 8 * i), LoadU32(base, lists + 8 * i + 4));
  }
  AppendMem(p, end, "A", base, a, 16);
  AppendMem(p, end, "B", base, b, 16);
  __imp__sub_8256F458(ctx, base);
  const bool zero = LoadU32(base, out) == 0 && LoadU32(base, out + 4) == 0 && LoadU32(base, out + 16) == 0 &&
                    LoadU32(base, out + 20) == 0;
  static int sane = 0, logo = 0;
  const bool from_logo_path = lr == 0x8256FB30u && logo++ < 40;
  if (!zero && !from_logo_path && sane++ >= 40) return;
  if (!Take()) return;
  AppendMem(p, end, zero ? "| ZERO out" : "| out", base, out, 16);
  std::fprintf(stderr, "%s\n", line);
  std::fflush(stderr);
}
