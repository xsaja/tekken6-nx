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

#include <rex/logging.h>
#include <rex/stream.h>
#include <rex/sys_counters.h>
#include <rex/system/xevent.h>
#include <rex/system/hang_snapshot.h>
#include <rex/system/xthread.h>
#include <rex/cvar.h>

#include <cstdio>
#include <string>
#include <vector>

REXCVAR_DEFINE_STRING(thread_pulse_latch_names, "RainCondvar", "Threading",
                      "Comma-separated guest event names whose PulseEvent is latched when nobody waits: the next "
                      "WaitForSingleObject or SignalObjectAndWait on the event takes the latch and returns at once. "
                      "Tekken 6's rain jobs pulse RainCondvar without holding RainMutex and wait on it with no "
                      "predicate (sub_822E1AA0 / sub_822E1B28): a pulse that landed before the waiter was armed "
                      "froze the rain stages at 0 fps. Empty = off (a pulse with no waiter does nothing, as Windows)");

namespace rex::system {

namespace {
// tekken6-nx (2026-11-21, thread_pulse_latch_names): is `name` in the cvar's list? Parsed once.
bool PulseLatchWanted(const std::string& name) {
  static const std::vector<std::string> names = [] {
    std::vector<std::string> out;
    const std::string list = REXCVAR_GET(thread_pulse_latch_names);
    size_t start = 0;
    while (start <= list.size()) {
      size_t end = list.find(',', start);
      if (end == std::string::npos) end = list.size();
      size_t a = start, b = end;
      while (a < b && (list[a] == ' ' || list[a] == '\t')) ++a;
      while (b > a && (list[b - 1] == ' ' || list[b - 1] == '\t')) --b;
      if (b > a) out.emplace_back(list.substr(a, b - a));
      start = end + 1;
    }
    return out;
  }();
  if (name.empty()) return false;
  for (const auto& n : names) {
    if (n == name) return true;
  }
  return false;
}

// One stderr line per latch event: the first 20, then every 100th.
void PulseLatchReport(const char* name, int kind, uint64_t total) {
  if (total > 20 && total % 100 != 0) return;
  if (kind == 0) {
    std::fprintf(stderr, "[pulse_latch] %s: pulse with no waiter latched (%llu total)\n", name,
                 static_cast<unsigned long long>(total));
  } else {
    std::fprintf(stderr, "[pulse_latch] %s: consumed by guest thread %u (%llu total)\n", name,
                 XThread::IsInThread() ? XThread::GetCurrentThreadId() : 0u,
                 static_cast<unsigned long long>(total));
  }
}
}  // namespace

XEvent::XEvent(KernelState* kernel_state) : XObject(kernel_state, kObjectType) {}

XEvent::~XEvent() = default;

void XEvent::Initialize(bool manual_reset, bool initial_state) {
  assert_false(event_);

  this->CreateNative<X_KEVENT>();

  if (manual_reset) {
    event_ = rex::thread::Event::CreateManualResetEvent(initial_state);
  } else {
    event_ = rex::thread::Event::CreateAutoResetEvent(initial_state);
  }
  assert_not_null(event_);
}

void XEvent::InitializeNative(void* native_ptr, X_DISPATCH_HEADER* header) {
  assert_false(event_);

  switch (header->type) {
    case 0x00:  // EventNotificationObject (manual reset)
      manual_reset_ = true;
      break;
    case 0x01:  // EventSynchronizationObject (auto reset)
      manual_reset_ = false;
      break;
    default:
      assert_always();
      return;
  }

  bool initial_state = header->signal_state ? true : false;
  if (manual_reset_) {
    event_ = rex::thread::Event::CreateManualResetEvent(initial_state);
  } else {
    event_ = rex::thread::Event::CreateAutoResetEvent(initial_state);
  }
  assert_not_null(event_);
}

void XEvent::Query(uint32_t* out_type, uint32_t* out_state) {
  if (out_type) {
    *out_type = manual_reset_ ? 0x00 : 0x01;
  }
  if (out_state) {
    // Query the live host event, not the stale guest header
    auto result = rex::thread::Wait(event_.get(), false, std::chrono::milliseconds(0));
    if (result == rex::thread::WaitResult::kSuccess) {
      *out_state = 1;
      // Re-signal since we consumed the signal by waiting
      event_->Set();
    } else {
      *out_state = 0;
    }
  }
}

int32_t XEvent::Set(uint32_t priority_increment, bool wait) {
  REX_SYS_CENSUS(rex::syscount::kCensusEventSet, 1);
  NoteSignalOp(hang::kOpSet);  // tekken6-nx (2026-11-21, tekken_hang_snapshot)
  event_->Set();
  return 1;
}

int32_t XEvent::Pulse(uint32_t priority_increment, bool wait) {
  NoteSignalOp(hang::kOpPulse);
  // tekken6-nx (2026-11-21, thread_pulse_latch_names): the name is matched once, at the first pulse (the name is set
  // after Initialize, by SetAttributes).
  if (!pulse_latch_checked_.load(std::memory_order_acquire) &&
      !pulse_latch_checked_.exchange(true, std::memory_order_acq_rel)) {
    if (PulseLatchWanted(name())) {
      event_->EnablePulseLatch(name().c_str(), &PulseLatchReport);
      std::fprintf(stderr, "[pulse_latch] %s: latch enabled (handle %08X)\n", name().c_str(), handle());
    }
  }
  event_->Pulse();
  return 1;
}

int32_t XEvent::Reset() {
  NoteSignalOp(hang::kOpReset);
  event_->Reset();
  return 1;
}

void XEvent::Clear() {
  NoteSignalOp(hang::kOpReset);
  event_->Reset();
}

bool XEvent::Save(stream::ByteStream* stream) {
  REXSYS_DEBUG("XEvent {:08X} ({})", handle(), manual_reset_ ? "manual" : "auto");
  SaveObject(stream);

  bool signaled = true;
  auto result = rex::thread::Wait(event_.get(), false, std::chrono::milliseconds(0));
  if (result == rex::thread::WaitResult::kSuccess) {
    signaled = true;
  } else if (result == rex::thread::WaitResult::kTimeout) {
    signaled = false;
  } else {
    assert_always();
  }

  if (signaled) {
    // Reset the event in-case it's an auto-reset.
    event_->Set();
  }

  stream->Write<bool>(signaled);
  stream->Write<bool>(manual_reset_);

  return true;
}

object_ref<XEvent> XEvent::Restore(KernelState* kernel_state, stream::ByteStream* stream) {
  auto evt = new XEvent(nullptr);
  evt->kernel_state_ = kernel_state;

  evt->RestoreObject(stream);
  bool signaled = stream->Read<bool>();
  evt->manual_reset_ = stream->Read<bool>();

  if (evt->manual_reset_) {
    evt->event_ = rex::thread::Event::CreateManualResetEvent(false);
  } else {
    evt->event_ = rex::thread::Event::CreateAutoResetEvent(false);
  }
  assert_not_null(evt->event_);

  if (signaled) {
    evt->event_->Set();
  }

  return object_ref<XEvent>(evt);
}

}  // namespace rex::system
