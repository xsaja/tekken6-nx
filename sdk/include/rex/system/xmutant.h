#pragma once
/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2013 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 *
 * @modified    Tom Clay, 2026 - Adapted for ReXGlue runtime
 */

#include <rex/system/xobject.h>
#include <rex/system/xtypes.h>
#include <rex/thread.h>

namespace rex::system {

class XThread;

class XMutant : public XObject {
 public:
  static const XObject::Type kObjectType = XObject::Type::Mutant;

  explicit XMutant(KernelState* kernel_state);
  ~XMutant() override;

  void Initialize(bool initial_owner);
  void InitializeNative(void* native_ptr, X_DISPATCH_HEADER* header);

  X_STATUS ReleaseMutant(uint32_t priority_increment, bool abandon, bool wait);

  // tekken6-nx (2026-11-21, tekken_hang_snapshot): the owner's guest thread id as last set (0 = none).
  uint32_t owner_tid() const { return owner_tid_.load(std::memory_order_relaxed); }
  // tekken6-nx (2026-11-21, diagnostics): called by XObject::SignalAndWait before it releases this mutant as its
  // signal half: forgets the recorded owner when that release frees the mutant (the calling thread holds it once), so
  // a hang snapshot shows the real owner. Diagnostic only; the release itself is done by rex::thread::SignalAndWait.
  void NoteSignalAndWaitRelease();

  bool Save(stream::ByteStream* stream) override;
  static object_ref<XMutant> Restore(KernelState* kernel_state, stream::ByteStream* stream);

 protected:
  rex::thread::WaitHandle* GetWaitHandle() override { return mutant_.get(); }
  void WaitCallback() override;

 private:
  XMutant();

  std::unique_ptr<rex::thread::Mutant> mutant_;
  XThread* owning_thread_ = nullptr;
  std::atomic<uint32_t> owner_tid_{0};
};

}  // namespace rex::system
