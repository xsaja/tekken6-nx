/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2022 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 *
 * @modified    Tom Clay, 2026 - Adapted for ReXGlue runtime
 */

#include <vector>

#include <rex/chrono/clock.h>
#include <rex/stream.h>
#include <rex/system/kernel_state.h>
#include <rex/system/util/string_utils.h>  // For TranslateAnsiStringAddress
#include <rex/system/xobject.h>
// #include <rex/kernel/xboxkrnl/private.h>  // TODO: JIT only
#include <rex/system/hang_snapshot.h>
#include <rex/system/xenumerator.h>
#include <rex/system/xevent.h>
#include <rex/system/xfile.h>
#include <rex/system/xmodule.h>
#include <rex/system/xmutant.h>
#include <rex/system/xnotifylistener.h>
#include <rex/system/xsemaphore.h>
#include <rex/system/xsymboliclink.h>
#include <rex/system/xthread.h>
#include <rex/sys_counters.h>

#include "global_lock_spin.h"

#if REX_PLATFORM_SWITCH
#include <cstdio>
#include <cstdlib>

#include <rex/cvar.h>
#include <rex/logging.h>

#include "../core/threading_switch.h"
#endif

// tekken6-nx (2026-11-23): every guest thread runs at the same host priority (ignore_thread_priorities), so on four busy
// cores the frame's chain (the main thread, and the 2D drawing worker it waits for on one semaphore every frame, ~3 ms
// a Cemetery frame) shares its cores evenly with the skinning, rain and audio threads. With a priority set, a guest thread
// that waits on or releases that object takes it (once, at its first such call): it preempts the other guest threads.
REXCVAR_DEFINE_INT32(tekken_frame_chain_priority, 0, "Tekken",
                     "Host priority (Horizon, lower is higher; guest threads run at 0x3B) for the guest threads that "
                     "wait on or release tekken_frame_chain_object; 0 = off")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_STRING(tekken_frame_chain_object, "F8000080", "Tekken",
                      "Handle (hex) of the object whose waiters and releasers tekken_frame_chain_priority raises: the "
                      "semaphore the main thread waits on for the 2D drawing worker every frame")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);

// tekken6-nx (2026-11-24): with the frame chain raised, the console's audio broke up in fights (~200 mixer requests
// without data every 10 s, 0 before): the game's own sound threads, which the game raises above normal
// (KeSetBasePriorityThread, ignored here), stayed at 0x3B under the raised threads. A guest thread whose guest priority
// is above 0 runs at this host priority instead (applied by the thread itself at its next wait, as the game changes it).
REXCVAR_DEFINE_INT32(tekken_guest_priority_raise, 0, "Tekken",
                     "Host priority (Horizon, lower is higher) for guest threads the game gives a priority above "
                     "normal (sound and other short wake-ups); 0 = off (every guest thread at 0x3B)")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
// tekken6-nx (2026-11-24): raising every such thread also raised two busy ones above the frame chain: the 2D drawing
// worker (guest priority 59, ~24 % of a core) cost the main thread ~1 ms a frame (Lili vs Panda 54.9-56.1 -> 52-53
// fps). Guest priorities up to this one (the sound threads: 1-16) get tekken_guest_priority_raise; higher ones stay
// at 0x3B, where they were before (at the frame chain's 0x3A the 2D worker still cost the same: 52-54 fps).
REXCVAR_DEFINE_INT32(tekken_guest_priority_raise_max, 16, "Tekken",
                     "The highest guest priority tekken_guest_priority_raise applies to; above it a guest thread stays "
                     "at 0x3B")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);

namespace rex::system {

namespace {
#if REX_PLATFORM_SWITCH
void FollowGuestPriority() {
  static const int32_t raise = REXCVAR_GET(tekken_guest_priority_raise);
  if (!raise || !XThread::IsInThread())
    return;
  static const int32_t raise_max = REXCVAR_GET(tekken_guest_priority_raise_max);
  thread_local int32_t applied = 0x3B;  // the host priority last applied for the guest one
  XThread *thread = XThread::GetCurrentThread();
  const int32_t guest_priority = thread->priority();
  const int32_t host = guest_priority > 0 && guest_priority <= raise_max ? raise : 0x3B;
  if (host == applied)
    return;
  applied = host;
  const bool ok = RexSwitchSetCurrentThreadPriorityOk(host);
  std::fprintf(stderr, "[guest_priority] guest thread %08X (%s) priority %d: host 0x%X %s\n",
               unsigned(thread->thread_id()), thread->thread_name().c_str(), thread->priority(), unsigned(host),
               ok ? "set" : "FAILED");
}
#endif
}  // namespace

void NoteFrameChainObject(uint32_t handle) {
#if REX_PLATFORM_SWITCH
  FollowGuestPriority();
  static const int32_t priority = REXCVAR_GET(tekken_frame_chain_priority);
  if (!priority)
    return;
  static const uint32_t target = uint32_t(std::strtoul(REXCVAR_GET(tekken_frame_chain_object).c_str(), nullptr, 16));
  thread_local bool raised = false;
  if (raised || handle != target)
    return;
  raised = true;
  const bool ok = RexSwitchSetCurrentThreadPriorityOk(priority);
  const XThread *thread = XThread::IsInThread() ? XThread::GetCurrentThread() : nullptr;
  REXSYS_INFO("[frame_chain] priority 0x{:X} for guest thread {:08X} ({}): {}", priority,
              thread ? thread->thread_id() : 0, thread ? thread->thread_name() : std::string("?"), ok ? "set" : "FAILED");
  std::fprintf(stderr, "[frame_chain] priority 0x%X for guest thread %08X (%s): %s\n", unsigned(priority),
               thread ? unsigned(thread->thread_id()) : 0u, thread ? thread->thread_name().c_str() : "?",
               ok ? "set" : "FAILED");
#else
  (void)handle;
#endif
}

XObject::XObject(Type type) : kernel_state_(nullptr), pointer_ref_count_(1), type_(type) {
  handles_.reserve(10);
}

XObject::XObject(KernelState* kernel_state, Type type)
    : kernel_state_(kernel_state),
      type_(type),
      pointer_ref_count_(1),
      guest_object_ptr_(0),
      allocated_guest_object_(false) {
  handles_.reserve(10);

  // TODO: Assert kernel_state != nullptr in this constructor.
  if (kernel_state) {
    kernel_state->object_table()->AddHandle(this, nullptr);
  }
}

XObject::~XObject() {
  // TODO: these are being asserted true... find out why
  // assert_true(handles_.empty());
  // assert_zero(pointer_ref_count_);

  if (allocated_guest_object_) {
    uint32_t ptr = guest_object_ptr_ - sizeof(X_OBJECT_HEADER);
    auto header = memory()->TranslateVirtual<X_OBJECT_HEADER*>(ptr);

    // Free the object creation info
    if (header->object_type_ptr) {
      memory()->SystemHeapFree(header->object_type_ptr);
    }

    memory()->SystemHeapFree(ptr);
  }
}

rex::Runtime* XObject::emulator() const {
  return kernel_state_->emulator_;
}
KernelState* XObject::kernel_state() const {
  return kernel_state_;
}
rex::memory::Memory* XObject::memory() const {
  return kernel_state_->memory();
}

XObject::Type XObject::type() const {
  return type_;
}

void XObject::RetainHandle() {
  kernel_state_->object_table()->RetainHandle(handles_[0]);
}

bool XObject::ReleaseHandle() {
  // FIXME: Return true when handle is actually released.
  return kernel_state_->object_table()->ReleaseHandle(handles_[0]) == X_STATUS_SUCCESS;
}

X_STATUS XObject::Delete() {
  if (kernel_state_ == nullptr) {
    // Fake return value for api-scanner
    return X_STATUS_SUCCESS;
  } else {
    if (!name_.empty()) {
      kernel_state_->object_table()->RemoveNameMapping(name_);
    }
    return kernel_state_->object_table()->RemoveHandle(handles_[0]);
  }
}

bool XObject::SaveObject(stream::ByteStream* stream) {
  stream->Write<uint32_t>(allocated_guest_object_);
  stream->Write<uint32_t>(guest_object_ptr_);

  stream->Write(uint32_t(handles_.size()));
  stream->Write(&handles_[0], handles_.size() * sizeof(X_HANDLE));

  return true;
}

bool XObject::RestoreObject(stream::ByteStream* stream) {
  allocated_guest_object_ = stream->Read<uint32_t>() > 0;
  guest_object_ptr_ = stream->Read<uint32_t>();

  handles_.resize(stream->Read<uint32_t>());
  stream->Read(&handles_[0], handles_.size() * sizeof(X_HANDLE));

  // Restore our pointer to our handles in the object table.
  for (size_t i = 0; i < handles_.size(); i++) {
    kernel_state_->object_table()->RestoreHandle(handles_[i], this);
  }

  return true;
}

object_ref<XObject> XObject::Restore(KernelState* kernel_state, Type type,
                                     stream::ByteStream* stream) {
  switch (type) {
    case Type::Enumerator:
      break;
    case Type::Event:
      return XEvent::Restore(kernel_state, stream);
    case Type::File:
      return XFile::Restore(kernel_state, stream);
    case Type::IOCompletion:
      break;
    case Type::Module:
      return XModule::Restore(kernel_state, stream);
    case Type::Mutant:
      return XMutant::Restore(kernel_state, stream);
    case Type::NotifyListener:
      return XNotifyListener::Restore(kernel_state, stream);
    case Type::Semaphore:
      return XSemaphore::Restore(kernel_state, stream);
    case Type::Session:
      break;
    case Type::Socket:
      break;
    case Type::SymbolicLink:
      return XSymbolicLink::Restore(kernel_state, stream);
    case Type::Thread:
      return XThread::Restore(kernel_state, stream);
    case Type::Timer:
      break;
    case Type::Undefined:
      break;
  }

  assert_always("No restore handler exists for this object!");
  return nullptr;
}

void XObject::SetAttributes(uint32_t obj_attributes_ptr) {
  if (!obj_attributes_ptr) {
    return;
  }

  auto name = util::TranslateAnsiStringAddress(
      memory(),
      memory::load_and_swap<uint32_t>(memory()->TranslateVirtual(obj_attributes_ptr + 4)));
  if (!name.empty()) {
    name_ = std::string(name);
    kernel_state_->object_table()->AddNameMapping(name_, handles_[0]);
  }
}

uint32_t XObject::TimeoutTicksToMs(int64_t timeout_ticks) {
  if (timeout_ticks > 0) {
    // Absolute time, based on January 1, 1601.
    // TODO(benvanik): convert time to relative time.
    assert_always();
    return 0;
  } else if (timeout_ticks < 0) {
    // Relative time.
    return (uint32_t)(-timeout_ticks / 10000);  // Ticks -> MS
  } else {
    return 0;
  }
}


namespace {
// Records what the current guest thread is blocked on, for the hang dump.
struct DebugWaitScope {
  XThread* thread;
  DebugWaitScope(uint32_t a, uint32_t b = 0, uint32_t c = 0) : thread(XThread::GetCurrentThread()) {
    if (thread) {
      thread->SetDebugWait(a, b, c);
    }
  }
  ~DebugWaitScope() {
    if (thread) {
      thread->SetDebugWait(0, 0, 0);
    }
  }
};
// tekken6-nx (2026-11-21, tekken_hang_snapshot): the wait's timeout for the record (~0u: none).
uint32_t HangTimeout(std::chrono::milliseconds timeout_ms) {
  return timeout_ms == std::chrono::milliseconds::max() ? ~0u
                                                        : uint32_t(std::min<int64_t>(timeout_ms.count(), 0xFFFFFFFE));
}
}  // namespace


X_STATUS XObject::Wait(uint32_t wait_reason, uint32_t processor_mode, uint32_t alertable,
                       uint64_t* opt_timeout) {
  REX_SYS_CENSUS(rex::syscount::kCensusWait, 1);
  auto wait_handle = GetWaitHandle();
  if (!wait_handle) {
    // Object doesn't support waiting.
    return X_STATUS_SUCCESS;
  }
  DebugWaitScope debug_wait(handle());
  NoteFrameChainObject(handle());  // tekken6-nx (2026-11-23): tekken_frame_chain_priority

  auto timeout_ms = opt_timeout ? std::chrono::milliseconds(chrono::Clock::ScaleGuestDurationMillis(
                                      TimeoutTicksToMs(*opt_timeout)))
                                : std::chrono::milliseconds::max();

  XObject* const self = this;
  hang::Scope hang_scope(hang::kKindObj, 1, &self, HangTimeout(timeout_ms), alertable != 0);
  XThread::CheckTitleTermination();
  auto result = rex::thread::Wait(wait_handle, alertable ? true : false, timeout_ms);
  XThread::CheckTitleTermination();
  X_STATUS status;
  switch (result) {
    case rex::thread::WaitResult::kSuccess:
      WaitCallback();
      status = X_STATUS_SUCCESS;
      break;
    case rex::thread::WaitResult::kUserCallback:
      // Or X_STATUS_ALERTED?
      status = X_STATUS_USER_APC;
      break;
    case rex::thread::WaitResult::kTimeout:
      rex::thread::MaybeYield();
      status = X_STATUS_TIMEOUT;
      break;
    default:
    case rex::thread::WaitResult::kAbandoned:
    case rex::thread::WaitResult::kFailed:
      status = X_STATUS_ABANDONED_WAIT_0;
      break;
  }
  hang_scope.set_status(status);
  return status;
}

X_STATUS XObject::SignalAndWait(XObject* signal_object, XObject* wait_object, uint32_t wait_reason,
                                uint32_t processor_mode, uint32_t alertable,
                                uint64_t* opt_timeout) {
  auto timeout_ms = opt_timeout ? std::chrono::milliseconds(chrono::Clock::ScaleGuestDurationMillis(
                                      TimeoutTicksToMs(*opt_timeout)))
                                : std::chrono::milliseconds::max();

  DebugWaitScope debug_wait(wait_object->handle(), signal_object->handle());
  NoteFrameChainObject(0);  // tekken6-nx (2026-11-24): tekken_guest_priority_raise
  // tekken6-nx (2026-11-21, tekken_hang_snapshot): [0] the waited object, [1] the signalled one.
  XObject* const hang_objects[2] = {wait_object, signal_object};
  hang::Scope hang_scope(hang::kKindSigWait, 2, hang_objects, HangTimeout(timeout_ms), alertable != 0);
  signal_object->NoteSignalOp(hang::kOpSigWait);
  // tekken6-nx (2026-11-21, diagnostics): the signal half frees a mutant held once: forget its recorded owner first
  // (after the release another thread can take it and record itself).
  if (signal_object->type() == XObject::Type::Mutant) {
    static_cast<XMutant*>(signal_object)->NoteSignalAndWaitRelease();
  }
  auto result =
      rex::thread::SignalAndWait(signal_object->GetWaitHandle(), wait_object->GetWaitHandle(),
                                 alertable ? true : false, timeout_ms);
  X_STATUS status;
  switch (result) {
    case rex::thread::WaitResult::kSuccess:
      wait_object->WaitCallback();
      status = X_STATUS_SUCCESS;
      break;
    case rex::thread::WaitResult::kUserCallback:
      // Or X_STATUS_ALERTED?
      status = X_STATUS_USER_APC;
      break;
    case rex::thread::WaitResult::kTimeout:
      rex::thread::MaybeYield();
      status = X_STATUS_TIMEOUT;
      break;
    default:
    case rex::thread::WaitResult::kAbandoned:
    case rex::thread::WaitResult::kFailed:
      status = X_STATUS_ABANDONED_WAIT_0;
      break;
  }
  hang_scope.set_status(status);
  return status;
}

X_STATUS XObject::WaitMultiple(uint32_t count, XObject** objects, uint32_t wait_type,
                               uint32_t wait_reason, uint32_t processor_mode, uint32_t alertable,
                               uint64_t* opt_timeout) {
  std::vector<rex::thread::WaitHandle*> wait_handles(count);
  for (size_t i = 0; i < count; ++i) {
    wait_handles[i] = objects[i]->GetWaitHandle();
    assert_not_null(wait_handles[i]);
  }
  DebugWaitScope debug_wait(count > 0 ? objects[0]->handle() : 0,
                            count > 1 ? objects[1]->handle() : 0,
                            count > 2 ? objects[2]->handle() : 0);
  NoteFrameChainObject(0);  // tekken6-nx (2026-11-24): tekken_guest_priority_raise

  auto timeout_ms = opt_timeout ? std::chrono::milliseconds(chrono::Clock::ScaleGuestDurationMillis(
                                      TimeoutTicksToMs(*opt_timeout)))
                                : std::chrono::milliseconds::max();

  // tekken6-nx (2026-11-21, tekken_hang_snapshot): the original body, run as a lambda for the record's result.
  hang::Scope hang_scope(wait_type ? hang::kKindAny : hang::kKindAll, count, objects, HangTimeout(timeout_ms),
                         alertable != 0);
  const X_STATUS hang_status = [&]() -> X_STATUS {
  XThread::CheckTitleTermination();
  if (wait_type) {
    auto result =
        rex::thread::WaitAny(std::move(wait_handles), alertable ? true : false, timeout_ms);
    XThread::CheckTitleTermination();
    switch (result.first) {
      case rex::thread::WaitResult::kSuccess:
        objects[result.second]->WaitCallback();

        return X_STATUS(result.second);
      case rex::thread::WaitResult::kUserCallback:
        // Or X_STATUS_ALERTED?
        return X_STATUS_USER_APC;
      case rex::thread::WaitResult::kTimeout:
        rex::thread::MaybeYield();
        return X_STATUS_TIMEOUT;
      default:
      case rex::thread::WaitResult::kAbandoned:
        return X_STATUS(X_STATUS_ABANDONED_WAIT_0 + result.second);
      case rex::thread::WaitResult::kFailed:
        return X_STATUS_UNSUCCESSFUL;
    }
  } else {
    auto result =
        rex::thread::WaitAll(std::move(wait_handles), alertable ? true : false, timeout_ms);
    XThread::CheckTitleTermination();
    switch (result) {
      case rex::thread::WaitResult::kSuccess:
        for (uint32_t i = 0; i < count; i++) {
          objects[i]->WaitCallback();
        }

        return X_STATUS_SUCCESS;
      case rex::thread::WaitResult::kUserCallback:
        // Or X_STATUS_ALERTED?
        return X_STATUS_USER_APC;
      case rex::thread::WaitResult::kTimeout:
        rex::thread::MaybeYield();
        return X_STATUS_TIMEOUT;
      default:
      case rex::thread::WaitResult::kAbandoned:
      case rex::thread::WaitResult::kFailed:
        return X_STATUS_ABANDONED_WAIT_0;
    }
  }
  }();
  hang_scope.set_status(hang_status);
  return hang_status;
}

uint8_t* XObject::CreateNative(uint32_t size) {
  auto global_lock = rex::thread::global_critical_region::AcquireDirect();

  uint32_t total_size = size + sizeof(X_OBJECT_HEADER);

  auto mem = memory()->SystemHeapAlloc(total_size);
  if (!mem) {
    // Out of memory!
    return nullptr;
  }

  allocated_guest_object_ = true;
  memory()->Zero(mem, total_size);
  SetNativePointer(mem + sizeof(X_OBJECT_HEADER), true);

  auto header = memory()->TranslateVirtual<X_OBJECT_HEADER*>(mem);

  auto object_type = memory()->SystemHeapAlloc(sizeof(X_OBJECT_TYPE));
  if (object_type) {
    // Set it up in the header.
    // Some kernel method is accessing this struct and dereferencing a member
    // @ offset 0x14
    header->object_type_ptr = object_type;
  }

  return memory()->TranslateVirtual(guest_object_ptr_);
}

void XObject::SetNativePointer(uint32_t native_ptr, bool uninitialized) {
  auto global_lock = rex::thread::global_critical_region::AcquireDirect();

  // If hit: We've already setup the native ptr with CreateNative!
  assert_zero(guest_object_ptr_);

  auto header = kernel_state_->memory()->TranslateVirtual<X_DISPATCH_HEADER*>(native_ptr);

  // Memory uninitialized, so don't bother with the check.
  if (!uninitialized) {
    assert_true(!(header->wait_list_blink & 0x1));
  }

  // Stash pointer in struct.
  // FIXME: This assumes the object has a dispatch header (some don't!)
  StashHandle(header, handle());

  guest_object_ptr_ = native_ptr;
}

object_ref<XObject> XObject::GetNativeObject(KernelState* kernel_state, void* native_ptr,
                                             int32_t as_type) {
  assert_not_null(native_ptr);
  REX_SYS_CENSUS(rex::syscount::kCensusNativeObject, 1);

  // Unfortunately the XDK seems to inline some KeInitialize calls, meaning
  // we never see it and just randomly start getting passed events/timers/etc.
  // Luckily it seems like all other calls (Set/Reset/Wait/etc) are used and
  // we don't have to worry about PPC code poking the struct. Because of that,
  // we init on first use, store our handle in the struct, and dereference it
  // each time.
  // We identify this by setting wait_list_flink to a magic value. When set,
  // wait_list_blink will hold a handle to our object.

  auto global_lock = AcquireGlobalSpin(rex::thread::global_critical_region::mutex());

  auto header = reinterpret_cast<X_DISPATCH_HEADER*>(native_ptr);

  if (as_type == -1) {
    as_type = header->type;
  }

  if (header->wait_list_flink == kXObjSignature) {
    // Already initialized.
    // TODO: assert if the type of the object != as_type
    uint32_t handle = header->wait_list_blink;
    auto object = kernel_state->object_table()->LookupObject<XObject>(handle);

    // TODO(benvanik): assert nothing has been changed in the struct.
    return object;
  } else {
    // First use, create new.
    // https://www.nirsoft.net/kernel_struct/vista/KOBJECTS.html
    XObject* object = nullptr;
    switch (as_type) {
      case 0:  // EventNotificationObject
      case 1:  // EventSynchronizationObject
      {
        auto ev = new XEvent(kernel_state);
        ev->InitializeNative(native_ptr, header);
        object = ev;
      } break;
      case 2:  // MutantObject
      {
        auto mutant = new XMutant(kernel_state);
        mutant->InitializeNative(native_ptr, header);
        object = mutant;
      } break;
      case 5:  // SemaphoreObject
      {
        auto sem = new XSemaphore(kernel_state);
        auto success = sem->InitializeNative(native_ptr, header);
        // Can't report failure to the guest at late initialization:
        assert_true(success);
        object = sem;
      } break;
      case 3:   // ProcessObject
      case 4:   // QueueObject
      case 6:   // ThreadObject
      case 7:   // GateObject
      case 8:   // TimerNotificationObject
      case 9:   // TimerSynchronizationObject
      case 18:  // ApcObject
      case 19:  // DpcObject
      case 20:  // DeviceQueueObject
      case 21:  // EventPairObject
      case 22:  // InterruptObject
      case 23:  // ProfileObject
      case 24:  // ThreadedDpcObject
      default:
        assert_always();
        return NULL;
    }

    // Stash pointer in struct.
    // FIXME: This assumes the object contains a dispatch header (some don't!)
    StashHandle(header, object->handle());

    return object_ref<XObject>(object);
  }
}

}  // namespace rex::system
