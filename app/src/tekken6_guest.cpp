// Tekken 6 guest-side glue, from tekken6-recomp (GPL-3.0): the vtable tail-call thunks codegen does not emit as
// functions, and the bodies of the mid-asm hooks declared in app/overrides.toml (docs/tekken6-notes.md).
#include "generated/renut_pch.h"

#include <rex/logging.h>
#include <rex/memory/utils.h>
#include <rex/runtime.h>
#include <rex/system/function_dispatcher.h>

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <thread>

#include "tekken6_guest.h"

namespace {

constexpr PPCRegister PPCContext::* kGpr[32] = {
    &PPCContext::r0,  &PPCContext::r1,  &PPCContext::r2,  &PPCContext::r3,  &PPCContext::r4,  &PPCContext::r5,
    &PPCContext::r6,  &PPCContext::r7,  &PPCContext::r8,  &PPCContext::r9,  &PPCContext::r10, &PPCContext::r11,
    &PPCContext::r12, &PPCContext::r13, &PPCContext::r14, &PPCContext::r15, &PPCContext::r16, &PPCContext::r17,
    &PPCContext::r18, &PPCContext::r19, &PPCContext::r20, &PPCContext::r21, &PPCContext::r22, &PPCContext::r23,
    &PPCContext::r24, &PPCContext::r25, &PPCContext::r26, &PPCContext::r27, &PPCContext::r28, &PPCContext::r29,
    &PPCContext::r30, &PPCContext::r31};

// The retail four-instruction thunk: load the object's vtable (r3) into one register, the slot into another, mtctr,
// bctr. Both registers are visible to the callee, so they are written as the guest would.
template <uint32_t SlotOffset, uint32_t VtableRegister, uint32_t MethodRegister>
void VirtualCallThunk(PPCContext& ctx, uint8_t* base) {
  const uint32_t vtable = rex::memory::load_and_swap<uint32_t>(base + ctx.r3.u32);
  const uint32_t method = rex::memory::load_and_swap<uint32_t>(base + vtable + SlotOffset);
  (ctx.*kGpr[VtableRegister]).u64 = vtable;
  (ctx.*kGpr[MethodRegister]).u64 = method;
  ctx.ctr.u64 = method;
  REX_CALL_INDIRECT_FUNC(method);
}

struct Resolution {
  uint32_t width;
  uint32_t height;
};

}  // namespace

namespace tekken6 {

void InstallVtableThunks(rex::Runtime* runtime) {
  auto* dispatcher = runtime ? runtime->function_dispatcher() : nullptr;
  if (!dispatcher) return;
  uint32_t failed = 0;
#define T6_THUNK(address, slot, vreg, mreg)                                         \
  if (!dispatcher->SetFunction(address, &VirtualCallThunk<slot, vreg, mreg>)) {    \
    REXLOG_WARN("tekken6: could not register vtable thunk {:08X} (slot +{:X})", address, slot); \
    ++failed;                                                                       \
  }
#include "tekken6_vtable_thunks.inc"
#undef T6_THUNK
  REXLOG_INFO("tekken6: vtable thunks installed ({} failed)", failed);
}

}  // namespace tekken6

// Mid-asm hook 0x8251C1FC: the startup queue spins without yielding. TEKKEN6_SLEEP_STARTUP_SPIN /
// TEKKEN6_YIELD_STARTUP_SPIN pick a back-off; the fence keeps each iteration's load of the flag another host thread
// writes.
void Tekken6StartupWait(PPCRegister& spin) {
  static const bool sleep = std::getenv("TEKKEN6_SLEEP_STARTUP_SPIN") != nullptr;
  static const bool yield = std::getenv("TEKKEN6_YIELD_STARTUP_SPIN") != nullptr;
  if (spin.s32 > 0) {
    if (sleep) {
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
    } else if (yield) {
      std::this_thread::yield();
    }
  }
  std::atomic_signal_fence(std::memory_order_seq_cst);
}

// Mid-asm hooks 0x82219DB4 / 0x82219A28: right before the guest stores its 1024x576 internal 3D size.
void ApplyTekkenInternalResolution(PPCRegister& width, PPCRegister& height) {
  static const Resolution r = [] {
    const char* v = std::getenv("TEKKEN6_INTERNAL_RESOLUTION");
    if (v && std::strcmp(v, "640x360") == 0) return Resolution{640, 360};
    if (v && std::strcmp(v, "768x432") == 0) return Resolution{768, 432};
    return Resolution{1024, 576};
  }();
  width.u32 = r.width;
  height.u32 = r.height;
}

// Mid-asm hook 0x821AB410: the 1024x1024 offscreen target, resource id 0x20000004 only.
void ApplyTekkenOffscreenResolution(PPCRegister& resource_id, PPCRegister& width, PPCRegister& height) {
  if (resource_id.u32 != 0x20000004) return;
  static const Resolution r = [] {
    const char* v = std::getenv("TEKKEN6_OFFSCREEN_RESOLUTION");
    if (v && std::strcmp(v, "512x512") == 0) return Resolution{512, 512};
    return Resolution{1024, 1024};
  }();
  width.u32 = r.width;
  height.u32 = r.height;
}
