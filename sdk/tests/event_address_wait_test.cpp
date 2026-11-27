// Stress test of the event wait in sdk/src/core/threading_posix.cpp (PosixConditionBase::Wait / Arm / Disarm and
// PosixCondition<Event>::Signal / Reset / Pulse), with thread_event_address_wait (renut_nx_ultimate_base by
// ChanseyIsTheBest, docs/ultimate-fixes.md row 65) or the condition variable. The logic is repeated here so it runs on
// a PC; C++20 atomic wait / notify stand in for svcWaitForAddress(WaitIfEqual) / svcSignalToAddress. The console's
// 100 ms re-check is left out on purpose: a lost wake-up must show as a stall here, not be papered over.
//   clang++ -O2 -std=c++20 sdk/tests/event_address_wait_test.cpp -o out/tools/event_address_wait_test.exe
//   event_address_wait_test <handoff|pulse|setreset|saw|barrier|barrierset|rain|rainoff> [seconds=10] [cond|addr]
// handoff    auto-reset SetEvent hand-off: one producer, 6 consumers (two in 1 ms slices); every Set passes exactly
//            one consumer.
// pulse      manual-reset PulseEvent with 6 blocked waiters: every pulse releases all of them.
// setreset   manual-reset SetEvent + ResetEvent right away with 6 blocked waiters: Windows releases all of them
//            (the address-wait Set does; the condition variable can strand them, informational there).
// saw        SignalObjectAndWait ping-pong between two threads on two auto-reset events (two pairs at once).
// barrier    Tekken 6's guest condition variable: an auto-reset event as the mutex, SignalObjectAndWait(mutex, cv)
//            and the last of 6 threads PulseEvent(cv) (manual-reset) while holding the mutex.
// barrierset the same barrier with two manual-reset events alternated (Set this phase's, Reset the other's).
// rain       Tekken 6's rain jobs (sub_822E1AA0 / sub_822E1B28), with the pulse latch (thread_pulse_latch_names): one
//            "phase 2" thread locks the mutex, sleeps 0-2 ms, SignalAndWait(mutex, cv) with no predicate, relocks and
//            unlocks; 6 "phase 1" threads, the last of which PulseEvent(cv) WITHOUT the mutex; a controller waits for
//            both, round after round. A round over 2 s is a stall.
// rainoff    the same without the latch: a pulse that lands before phase 2 is armed is lost and it stalls.
#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <mutex>
#include <random>
#include <thread>
#include <vector>

namespace {

bool g_addr = false;
bool g_latch = false;  // thread_pulse_latch_names, on the events that enable it
using Clock = std::chrono::steady_clock;
using ms = std::chrono::milliseconds;

class Event {
 public:
  struct Waiter {
    bool granted = false;
  };
  Event(bool manual_reset, bool initial) : signal_(initial), manual_reset_(manual_reset), handoff_(!manual_reset) {}
  void EnableLatch() { latch_on_ = true; }

  uint64_t Arm(Waiter* w) {
    std::lock_guard<std::mutex> lock(mutex_);
    ++armed_;
    if (handoff_) queue_.push_back(w);
    return pulse_gen_;
  }
  void Disarm(Waiter* w, bool consumed) {
    bool pass_on;
    {
      std::lock_guard<std::mutex> lock(mutex_);
      --armed_;
      Dequeue(w);
      pass_on = w->granted && !consumed;
      if (pass_on) GrantOrSet();
    }
    if (pass_on) WakeAllBlocked();
  }

  // true = success
  bool Wait(ms timeout, const uint64_t* armed_gen = nullptr, Waiter* armed = nullptr) {
    bool executed;
    uint64_t gen = 0;
    Waiter local;
    Waiter* me = armed ? armed : &local;
    auto predicate = [this, &gen, me] { return me->granted || signal_ || pulse_gen_ != gen; };
    const auto deadline = timeout == ms::max() ? Clock::time_point::max() : Clock::now() + timeout;
    std::unique_lock<std::mutex> lock(mutex_);
    if (latch_ != 0 && timeout.count() != 0) {  // a latched pulse: taken, success at once
      --latch_;
      ++latch_consumed_;
      return true;
    }
    gen = armed_gen ? *armed_gen : pulse_gen_;
    if (predicate()) {
      executed = true;
    } else {
      if (handoff_ && !armed) queue_.push_back(me);
      ++cond_waiters_;
      if (g_addr) {
        executed = AddressWaitLocked(lock, deadline, predicate);
      } else if (timeout == ms::max()) {
        cond_.wait(lock, predicate);
        executed = true;
      } else {
        executed = cond_.wait_until(lock, deadline, predicate);
      }
      --cond_waiters_;
      if (!armed && !me->granted) Dequeue(me);
    }
    if (me->granted) return true;
    if (executed) {
      if (signal_) {
        if (armed) Dequeue(me);
        if (!manual_reset_) signal_ = false;
        return true;
      }
      if (pulse_gen_ != gen) return true;
      return false;
    }
    return false;
  }

  void Set() {
    uint32_t waiters;
    {
      std::lock_guard<std::mutex> lock(mutex_);
      if (handoff_) {
        GrantOrSet();
      } else {
        signal_ = true;
        if (g_addr && cond_waiters_ + armed_ != 0) ++pulse_gen_;
      }
      waiters = cond_waiters_;
    }
    if (waiters != 0) WakeAllBlocked();
  }
  void Reset() {
    std::lock_guard<std::mutex> lock(mutex_);
    signal_ = false;
  }
  void Pulse() {
    {
      std::lock_guard<std::mutex> lock(mutex_);
      if (cond_waiters_ + armed_ == 0) {
        if (latch_on_) {
          ++latch_;
          ++latch_total_;
        }
        return;
      }
      if (manual_reset_) {
        ++pulse_gen_;
      } else {
        GrantOrSet();
      }
    }
    WakeAllBlocked();
  }
  uint32_t blocked() {
    std::lock_guard<std::mutex> lock(mutex_);
    return cond_waiters_;
  }
  void Dump(const char* name) {
    std::lock_guard<std::mutex> lock(mutex_);
    std::printf("  %s: signal=%d cond_waiters=%u armed=%u queue=%u pulse_gen=%llu seq=%u latch=%u latched=%llu "
                "consumed=%llu\n",
                name, int(signal_), cond_waiters_, armed_, unsigned(queue_.size()), (unsigned long long)pulse_gen_,
                seq_.load(), latch_, (unsigned long long)latch_total_, (unsigned long long)latch_consumed_);
  }
  uint64_t pulse_gen() {
    std::lock_guard<std::mutex> lock(mutex_);
    return pulse_gen_;
  }
  void LatchStats(uint32_t* pending, uint64_t* latched, uint64_t* consumed) {
    std::lock_guard<std::mutex> lock(mutex_);
    *pending = latch_;
    *latched = latch_total_;
    *consumed = latch_consumed_;
  }

 private:
  void WakeAllBlocked() {
    if (g_addr) {
      seq_.fetch_add(1, std::memory_order_seq_cst);
      seq_.notify_all();
    } else {
      cond_.notify_all();
    }
  }
  template <typename P>
  bool AddressWaitLocked(std::unique_lock<std::mutex>& lock, Clock::time_point deadline, P& pred) {
    for (;;) {
      if (pred()) return true;
      if (deadline != Clock::time_point::max() && Clock::now() >= deadline) return false;
      const uint32_t seen = seq_.load(std::memory_order_seq_cst);
      lock.unlock();
      if (deadline == Clock::time_point::max()) {
        seq_.wait(seen, std::memory_order_seq_cst);
      } else {
        // atomic wait has no timeout: short slices (the svc takes one)
        while (seq_.load(std::memory_order_seq_cst) == seen && Clock::now() < deadline) std::this_thread::yield();
      }
      lock.lock();
    }
  }
  void Dequeue(Waiter* w) {
    auto it = std::find(queue_.begin(), queue_.end(), w);
    if (it != queue_.end()) queue_.erase(it);
  }
  void GrantOrSet() {
    if (!queue_.empty()) {
      queue_.front()->granted = true;
      queue_.pop_front();
    } else {
      signal_ = true;
    }
  }

  std::mutex mutex_;
  std::condition_variable cond_;
  std::atomic<uint32_t> seq_{0};
  bool signal_;
  const bool manual_reset_;
  const bool handoff_;
  uint32_t cond_waiters_ = 0, armed_ = 0;
  uint64_t pulse_gen_ = 0;
  std::deque<Waiter*> queue_;
  bool latch_on_ = false;
  uint32_t latch_ = 0;
  uint64_t latch_total_ = 0, latch_consumed_ = 0;
};

// SignalObjectAndWait as rex::thread::SignalAndWait does it: arm, signal, wait, disarm.
bool SignalAndWait(Event& to_signal, Event& wait_on, ms timeout) {
  Event::Waiter waiter;
  const uint64_t gen = wait_on.Arm(&waiter);
  to_signal.Set();
  const bool ok = wait_on.Wait(timeout, &gen, &waiter);
  wait_on.Disarm(&waiter, ok);
  return ok;
}

[[noreturn]] void Stall(const char* what, uint64_t round) {
  std::printf("STALL (%s) at round %llu\n", what, (unsigned long long)round);
  std::fflush(stdout);
  std::_Exit(1);
}

// Waits up to 2 s for counter >= target.
bool WaitCount(const std::atomic<uint64_t>& counter, uint64_t target) {
  const auto end = Clock::now() + std::chrono::seconds(2);
  while (counter.load() < target) {
    if (Clock::now() >= end) return false;
    std::this_thread::yield();
  }
  return true;
}

// Waits up to 2 s for every one of n waiters to be blocked on e.
bool WaitBlocked(Event& e, uint32_t n) {
  const auto end = Clock::now() + std::chrono::seconds(2);
  while (e.blocked() < n) {
    if (Clock::now() >= end) return false;
    std::this_thread::yield();
  }
  return true;
}

constexpr int kThreads = 6;

uint64_t RunHandoff(Clock::time_point end) {
  Event evt(false, false), ack(false, false);
  std::atomic<bool> stop{false};
  std::atomic<uint64_t> passed{0};
  std::vector<std::thread> ts;
  for (int w = 0; w < kThreads; ++w) {
    ts.emplace_back([&, w] {
      std::mt19937 rng(w);
      while (!stop.load()) {
        const ms t = w < 2 ? ms(1) : ms::max();  // two alertable-style waiters in 1 ms slices
        if (!evt.Wait(t)) continue;
        passed.fetch_add(1);
        for (volatile int i = int(rng() % 100); i > 0; i = i - 1) {
        }
        ack.Set();
      }
    });
  }
  uint64_t rounds = 0;
  while (Clock::now() < end) {
    evt.Set();
    if (!ack.Wait(ms(2000))) {
      evt.Dump("evt");
      ack.Dump("ack");
      Stall("handoff", rounds);
    }
    ++rounds;
  }
  stop.store(true);
  for (int i = 0; i < kThreads; ++i) evt.Set();
  for (auto& t : ts) t.join();
  // every Set passes exactly one consumer: rounds + the stop Sets (some may stay set: at most one leftover)
  const uint64_t p = passed.load();
  if (p < rounds || p > rounds + kThreads) {
    std::printf("WRONG COUNT: %llu passes for %llu Sets\n", (unsigned long long)p, (unsigned long long)rounds);
    std::_Exit(1);
  }
  return rounds;
}

uint64_t RunPulseOrSetReset(Clock::time_point end, bool set_reset) {
  Event evt(true, false);
  std::atomic<bool> stop{false};
  std::atomic<uint64_t> passed{0};
  std::atomic<uint64_t> epoch{0};  // bumped by the producer once a round is over (after its Reset)
  std::vector<std::thread> ts;
  for (int w = 0; w < kThreads; ++w) {
    ts.emplace_back([&] {
      while (!stop.load()) {
        const uint64_t e = epoch.load();
        if (!evt.Wait(ms::max())) continue;
        passed.fetch_add(1);
        // one pass per round: a manual-reset event still set would let this thread through again
        while (epoch.load() == e && !stop.load()) std::this_thread::yield();
      }
    });
  }
  uint64_t rounds = 0;
  while (Clock::now() < end) {
    if (!WaitBlocked(evt, kThreads)) {
      evt.Dump("evt");
      Stall("waiters never blocked", rounds);
    }
    const uint64_t target = passed.load() + kThreads;
    if (set_reset) {
      evt.Set();
      evt.Reset();
    } else {
      evt.Pulse();
    }
    if (!WaitCount(passed, target)) {
      std::printf("  released %llu of %d\n", (unsigned long long)(passed.load() + kThreads - target), kThreads);
      evt.Dump("evt");
      Stall(set_reset ? "set+reset" : "pulse", rounds);
    }
    epoch.fetch_add(1);
    ++rounds;
  }
  stop.store(true);
  evt.Set();
  for (auto& t : ts) t.join();
  return rounds;
}

uint64_t RunSaw(Clock::time_point end) {
  std::atomic<bool> stop{false};
  std::atomic<uint64_t> total{0};
  std::atomic<bool> stalled{false};
  std::vector<std::thread> ts;
  struct Pair {
    Event a{false, false}, b{false, false};
  };
  Pair pairs[2];
  for (int p = 0; p < 2; ++p) {
    Pair& pr = pairs[p];
    // A: SignalAndWait(b, a) in a loop; B: waits b once, then SignalAndWait(a, b) in a loop.
    ts.emplace_back([&, p] {
      while (!stop.load()) {
        // one of the pairs uses timed waits (alertable slices), retried
        bool ok = SignalAndWait(pr.b, pr.a, p ? ms(1) : ms(2000));
        while (!ok && p && !stop.load()) ok = pr.a.Wait(ms(1));
        if (!ok && !stop.load()) {
          pr.a.Dump("a");
          pr.b.Dump("b");
          stalled.store(true);
          return;
        }
        total.fetch_add(1);
      }
    });
    ts.emplace_back([&, p] {
      if (!pr.b.Wait(ms(2000))) {
        stalled.store(true);
        return;
      }
      while (!stop.load()) {
        bool ok = SignalAndWait(pr.a, pr.b, p ? ms(1) : ms(2000));
        while (!ok && p && !stop.load()) ok = pr.b.Wait(ms(1));
        if (!ok && !stop.load()) {
          stalled.store(true);
          return;
        }
      }
    });
  }
  while (Clock::now() < end && !stalled.load()) std::this_thread::sleep_for(ms(50));
  if (stalled.load()) Stall("saw", total.load());
  stop.store(true);
  for (int i = 0; i < 4; ++i) {
    for (auto& pr : pairs) {
      pr.a.Set();
      pr.b.Set();
    }
    std::this_thread::sleep_for(ms(5));
  }
  for (auto& t : ts) t.join();
  return total.load();
}

uint64_t RunBarrier(Clock::time_point end, bool alternate) {
  Event mtx(false, true);  // auto-reset event used as a mutex (initially free)
  Event cv(true, false);   // manual-reset: pulsed (barrier) ...
  Event phase_evt[2] = {Event(true, false), Event(true, false)};  // ... or alternated (barrierset)
  std::atomic<bool> stop{false};
  std::atomic<bool> stalled{false};
  std::atomic<uint64_t> rounds{0};
  int arrived = 0;      // guarded by mtx
  uint64_t phase = 0;   // guarded by mtx
  std::vector<std::thread> ts;
  for (int w = 0; w < kThreads; ++w) {
    ts.emplace_back([&] {
      while (!stop.load()) {
        if (!mtx.Wait(ms(2000))) {
          stalled.store(true);
          return;
        }
        const uint64_t my_phase = phase;
        if (++arrived == kThreads) {
          arrived = 0;
          ++phase;
          rounds.fetch_add(1);
          if (alternate) {
            phase_evt[(my_phase + 1) & 1].Reset();
            phase_evt[my_phase & 1].Set();
          } else {
            cv.Pulse();  // every other thread is armed on cv: SignalAndWait armed it before freeing mtx
          }
          mtx.Set();
        } else {
          Event& wait_on = alternate ? phase_evt[my_phase & 1] : cv;
          if (!SignalAndWait(mtx, wait_on, ms(2000))) {
            if (!stop.load()) stalled.store(true);
            return;
          }
        }
      }
    });
  }
  uint64_t last = 0;
  auto last_change = Clock::now();
  while (Clock::now() < end && !stalled.load()) {
    std::this_thread::sleep_for(ms(50));
    const uint64_t r = rounds.load();
    if (r != last) {
      last = r;
      last_change = Clock::now();
    } else if (Clock::now() - last_change > std::chrono::seconds(2)) {
      stalled.store(true);
    }
  }
  if (stalled.load()) {
    mtx.Dump("mtx");
    cv.Dump("cv");
    phase_evt[0].Dump("phase0");
    phase_evt[1].Dump("phase1");
    Stall(alternate ? "barrierset" : "barrier", rounds.load());
  }
  stop.store(true);
  // release everyone: threads blocked on cv / phase events or on mtx
  for (int i = 0; i < 20; ++i) {
    cv.Set();
    phase_evt[0].Set();
    phase_evt[1].Set();
    mtx.Set();
    std::this_thread::sleep_for(ms(5));
  }
  for (auto& t : ts) t.join();
  return rounds.load();
}

// Tekken 6's rain jobs, round after round. The mutex is an auto-reset event (as in "barrier"; RainMutex is a mutant
// in the game, the protocol is the same: one owner, released by the signal half of SignalAndWait).
uint64_t RunRain(Clock::time_point end, int phase1_threads) {
  Event mtx(false, true);  // RainMutex (initially free)
  Event cv(true, false);   // RainCondvar: manual-reset, only ever pulsed
  if (g_latch) cv.EnableLatch();
  std::atomic<uint64_t> round_go{0};  // the controller starts round r by storing r
  std::atomic<uint64_t> p1_done{0}, p2_done{0};
  std::atomic<int> arrived{0};
  std::atomic<bool> stop{false};
  std::vector<std::thread> ts;
  // phase 2
  ts.emplace_back([&] {
    std::mt19937 rng(1234);
    uint64_t seen = 0;
    while (!stop.load()) {
      const uint64_t r = round_go.load();
      if (r == seen) {
        std::this_thread::yield();
        continue;
      }
      seen = r;
      if (!mtx.Wait(ms::max())) return;
      std::this_thread::sleep_for(std::chrono::microseconds(rng() % 2001));  // preempted between lock and wait
      SignalAndWait(mtx, cv, ms::max());  // no predicate
      if (stop.load()) return;
      mtx.Wait(ms::max());  // relock, "process", unlock
      mtx.Set();
      p2_done.store(r);
    }
  });
  // phase 1
  for (int w = 0; w < phase1_threads; ++w) {
    ts.emplace_back([&, w] {
      std::mt19937 rng(w);
      uint64_t seen = 0;
      while (!stop.load()) {
        const uint64_t r = round_go.load();
        if (r == seen) {
          std::this_thread::yield();
          continue;
        }
        seen = r;
        for (volatile int i = int(rng() % 20000); i > 0; i = i - 1) {
        }
        if (arrived.fetch_add(1) + 1 == phase1_threads) {
          arrived.store(0);
          cv.Pulse();  // the last job pulses, without the mutex
        }
        p1_done.fetch_add(1);
      }
    });
  }
  uint64_t rounds = 0;
  auto worst = Clock::duration::zero();
  while (Clock::now() < end) {
    const uint64_t r = rounds + 1;
    const uint64_t p1_target = r * phase1_threads;
    const auto t0 = Clock::now();
    round_go.store(r);
    const auto deadline = t0 + std::chrono::seconds(2);
    while ((p1_done.load() < p1_target || p2_done.load() < r) && Clock::now() < deadline) std::this_thread::yield();
    if (p1_done.load() < p1_target || p2_done.load() < r) {
      mtx.Dump("RainMutex");
      cv.Dump("RainCondvar");
      Stall(g_latch ? "rain, latch on" : "rain, latch off", rounds);
    }
    worst = std::max(worst, Clock::now() - t0);
    rounds = r;
  }
  stop.store(true);
  for (auto& t : ts) t.join();  // every thread is idle between rounds (phase 2 finished the last one)
  uint32_t pending;
  uint64_t latched, consumed;
  cv.LatchStats(&pending, &latched, &consumed);
  std::printf("  rounds=%llu pulses delivered to a waiter=%llu latched=%llu consumed=%llu pending=%u "
              "worst round=%.2f ms\n",
              (unsigned long long)rounds, (unsigned long long)cv.pulse_gen(), (unsigned long long)latched,
              (unsigned long long)consumed, pending, std::chrono::duration<double, std::milli>(worst).count());
  // exactly one pulse per round, each either delivered or latched and consumed
  if (latched != consumed || pending != 0 || cv.pulse_gen() + latched != rounds) {
    std::printf("WRONG LATCH COUNT\n");
    std::_Exit(1);
  }
  return rounds;
}

}  // namespace

int main(int argc, char** argv) {
  if (argc < 2) {
    std::printf("usage: %s <handoff|pulse|setreset|saw|barrier|barrierset|rain|rainoff> [seconds=10] [cond|addr]\n",
                argv[0]);
    return 2;
  }
  const char* mode = argv[1];
  const int seconds = argc > 2 ? std::atoi(argv[2]) : 10;
  g_addr = !(argc > 3 && std::strcmp(argv[3], "cond") == 0);
  const auto end = Clock::now() + std::chrono::seconds(seconds);
  uint64_t n = 0;
  const char* unit = "rounds";
  if (!std::strcmp(mode, "handoff")) {
    n = RunHandoff(end);
  } else if (!std::strcmp(mode, "pulse")) {
    n = RunPulseOrSetReset(end, false);
  } else if (!std::strcmp(mode, "setreset")) {
    n = RunPulseOrSetReset(end, true);
  } else if (!std::strcmp(mode, "saw")) {
    n = RunSaw(end);
    unit = "ping-pongs";
  } else if (!std::strcmp(mode, "barrier")) {
    n = RunBarrier(end, false);
  } else if (!std::strcmp(mode, "barrierset")) {
    n = RunBarrier(end, true);
  } else if (!std::strcmp(mode, "rain") || !std::strcmp(mode, "rainoff")) {
    g_latch = !std::strcmp(mode, "rain");
    n = RunRain(end, 6);
  } else {
    std::printf("unknown mode %s\n", mode);
    return 2;
  }
  std::printf("%s (%s): %llu %s in %d s, ok\n", mode, g_addr ? "addr" : "cond", (unsigned long long)n, unit,
              seconds);
  return 0;
}
