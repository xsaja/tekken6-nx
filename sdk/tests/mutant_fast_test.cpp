// Stress test of the mutant fast paths in sdk/src/core/threading_posix.cpp (PosixCondition<Mutant> with
// thread_mutant_acquire_fast and thread_mutant_release_lockfree; the latter from renut_nx_ultimate_base by
// ChanseyIsTheBest, docs/ultimate-fixes.md row 146). The base Wait() / WaitMultiple poll / Arm-Disarm logic and the
// mutant are repeated here so they run on a PC (the condition variable; mutants do not use the address wait).
//   clang++ -O2 -std=c++20 sdk/tests/mutant_fast_test.cpp -o out/tools/mutant_fast_test.exe
//   mutant_fast_test <solo|lock|condvar|all> [seconds=5] [old|new|fastonly|lockfreeonly]
// old          the previous mutant: count / owner under the mutex, notify_all at every release that frees it.
// new          both fast paths (the shipped defaults).
// fastonly     thread_mutant_acquire_fast only; lockfreeonly: thread_mutant_release_lockfree only.
// solo         one thread, lock / unlock pairs (with one recursive level every 4th): ns per pair.
// lock         6 threads: lock with recursion 1-3 (infinite waits, 1 ms timed waits, zero-timeout polls, and a
//              WaitAny on {mutant, idle event}), a non-atomic counter and an "inside" flag check mutual exclusion.
// condvar      the guest condition variable: 3 producers lock / push / unlock / PulseEvent(cv, manual reset); 3
//              consumers lock, while empty SignalObjectAndWait(mutant, cv) and relock, pop, unlock. Every item
//              pushed is popped exactly once. No progress for 2 s = a stall (a lost wake-up), reported and failed.
#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <mutex>
#include <random>
#include <thread>
#include <vector>

namespace {

bool g_fast = true;
bool g_lockfree = true;
bool g_old = false;
using Clock = std::chrono::steady_clock;
using ms = std::chrono::milliseconds;
std::atomic<uint64_t> g_idle_releases{0}, g_notifies{0}, g_fast_hits{0}, g_slow_waits{0};

enum class WaitResult { kSuccess, kTimeout };

// ---- PosixConditionBase (the parts a mutant uses) ----
class Cond {
 public:
  virtual ~Cond() = default;
  struct Waiter {
    bool granted = false;
  };
  uint64_t Arm(Waiter*) {
    std::lock_guard<std::mutex> lock(mutex_);
    ++armed_;
    return pulse_gen_;
  }
  void Disarm(Waiter*, bool) {
    std::lock_guard<std::mutex> lock(mutex_);
    --armed_;
  }
  void RegisterMulti() { multi_count_.fetch_add(1, std::memory_order_seq_cst); }
  void UnregisterMulti() { multi_count_.fetch_sub(1, std::memory_order_seq_cst); }
  void NotifyMulti() {
    if (multi_count_.load(std::memory_order_seq_cst) == 0) return;
    std::lock_guard<std::mutex> lock(g_multi_m);
    ++g_multi_seq;
    g_multi_cv.notify_all();
  }
  static std::mutex g_multi_m;
  static std::condition_variable g_multi_cv;
  static uint64_t g_multi_seq;

  WaitResult Wait(ms timeout, const uint64_t* armed_gen = nullptr, Waiter* armed = nullptr) {
    if (fast_acquire_ && armed == nullptr && TryFastAcquire()) {
      g_fast_hits.fetch_add(1, std::memory_order_relaxed);
      return WaitResult::kSuccess;
    }
    g_slow_waits.fetch_add(1, std::memory_order_relaxed);
    SlowSection slow(this);
    uint64_t gen = 0;
    auto predicate = [this, &gen] { return this->signaled() || pulse_gen_ != gen; };
    const auto deadline = timeout == ms::max() ? Clock::time_point::max() : Clock::now() + timeout;
    std::unique_lock<std::mutex> lock(mutex_);
    gen = armed_gen ? *armed_gen : pulse_gen_;
    bool executed;
    if (predicate()) {
      executed = true;
    } else {
      ++cond_waiters_;
      if (timeout == ms::max()) {
        cond_.wait(lock, predicate);
        executed = true;
      } else {
        executed = cond_.wait_until(lock, deadline, predicate);
      }
      --cond_waiters_;
    }
    if (executed) {
      if (signaled()) {
        post_execution();
        return WaitResult::kSuccess;
      }
      if (pulse_gen_ != gen) return WaitResult::kSuccess;
    }
    return WaitResult::kTimeout;
  }

  // WaitAny over handles (the legacy 1 ms poll / wake point of PosixConditionBase::WaitMultiple, simplified).
  static std::pair<WaitResult, size_t> WaitAny(std::vector<Cond*> handles, ms timeout) {
    for (auto* h : handles) h->RegisterMulti();
    for (auto* h : handles) {
      if (h->slow_hooks_) h->SlowEnter();
    }
    const auto end = timeout == ms::max() ? Clock::time_point::max() : Clock::now() + timeout;
    std::pair<WaitResult, size_t> result{WaitResult::kTimeout, 0};
    std::vector<std::unique_lock<std::mutex>> locks;
    for (;;) {
      uint64_t seen;
      {
        std::lock_guard<std::mutex> l(g_multi_m);
        seen = g_multi_seq;
      }
      bool all = true;
      for (auto* h : handles) {
        locks.emplace_back(h->mutex_, std::try_to_lock);
        if (!locks.back().owns_lock()) {
          all = false;
          break;
        }
      }
      if (all) {
        bool met = false;
        for (size_t i = 0; i < handles.size(); ++i) {
          if (handles[i]->signaled()) {
            handles[i]->post_execution();
            result = {WaitResult::kSuccess, i};
            met = true;
            break;
          }
        }
        if (met) break;
      }
      locks.clear();
      const auto now = Clock::now();
      if (now >= end) break;
      std::unique_lock<std::mutex> park(g_multi_m);
      g_multi_cv.wait_until(park, std::min(end, now + ms(all ? 20 : 1)), [&] { return g_multi_seq != seen; });
    }
    locks.clear();
    for (auto* h : handles) {
      if (h->slow_hooks_) h->SlowLeave();
    }
    for (auto* h : handles) h->UnregisterMulti();
    return result;
  }

  virtual bool Signal() = 0;

 protected:
  virtual bool signaled() const = 0;
  virtual void post_execution() = 0;
  bool slow_hooks_ = false;
  bool fast_acquire_ = false;
  virtual void SlowEnter() {}
  virtual void SlowLeave() {}
  virtual bool TryFastAcquire() { return false; }
  struct SlowSection {
    Cond* c;
    explicit SlowSection(Cond* cond) : c(cond->slow_hooks_ ? cond : nullptr) {
      if (c) c->SlowEnter();
    }
    ~SlowSection() {
      if (c) c->SlowLeave();
    }
  };
  void WakeWaiters(uint32_t waiters) {
    if (waiters != 0) {
      g_notifies.fetch_add(1, std::memory_order_relaxed);
      cond_.notify_all();
    } else {
      g_idle_releases.fetch_add(1, std::memory_order_relaxed);
    }
  }
  std::condition_variable cond_;
  std::mutex mutex_;
  uint32_t cond_waiters_ = 0;
  uint32_t armed_ = 0;
  uint64_t pulse_gen_ = 0;

 private:
  std::atomic<uint32_t> multi_count_{0};
};
std::mutex Cond::g_multi_m;
std::condition_variable Cond::g_multi_cv;
uint64_t Cond::g_multi_seq = 0;

// ---- the previous mutant ----
class OldMutant : public Cond {
 public:
  bool Signal() override { return Release(); }
  bool Release() {
    if (owner_ == std::this_thread::get_id() && count_ > 0) {
      bool released;
      {
        std::lock_guard<std::mutex> lock(mutex_);
        --count_;
        released = count_ == 0;
      }
      if (released) {
        g_notifies.fetch_add(1, std::memory_order_relaxed);
        cond_.notify_all();
        NotifyMulti();
      }
      return true;
    }
    return false;
  }

 private:
  bool signaled() const override { return count_ == 0 || owner_ == std::this_thread::get_id(); }
  void post_execution() override {
    count_++;
    owner_ = std::this_thread::get_id();
  }
  uint32_t count_ = 0;
  std::thread::id owner_;
};

// ---- the new mutant (a copy of PosixCondition<Mutant>; keep in step) ----
class NewMutant : public Cond {
 public:
  NewMutant() {
    slow_hooks_ = true;
    fast_acquire_ = g_fast;
  }
  bool Signal() override { return Release(); }
  bool Release() {
    const uint64_t me = ThreadKey();
    if ((state_.load(std::memory_order_relaxed) & kOwnerMask) != me) return false;
    const uint32_t count = count_.load(std::memory_order_relaxed);
    if (count == 0) return false;
    if (count > 1) {
      count_.store(count - 1, std::memory_order_relaxed);
      return true;
    }
    count_.store(0, std::memory_order_relaxed);
    if (!g_lockfree) {
      {
        std::lock_guard<std::mutex> lock(mutex_);
        state_.fetch_and(~kOwnerMask, std::memory_order_seq_cst);
      }
      g_notifies.fetch_add(1, std::memory_order_relaxed);
      cond_.notify_all();
      NotifyMulti();
      return true;
    }
    const uint64_t prev = state_.fetch_and(~kOwnerMask, std::memory_order_seq_cst);
    if ((prev >> 32) != 0) {
      uint32_t waiters;
      {
        std::lock_guard<std::mutex> lock(mutex_);
        waiters = cond_waiters_;
      }
      WakeWaiters(waiters);
    } else {
      g_idle_releases.fetch_add(1, std::memory_order_relaxed);
    }
    NotifyMulti();
    return true;
  }

 private:
  static constexpr uint64_t kOwnerMask = 0xFFFFFFFFull;
  static constexpr uint64_t kSlowOne = 1ull << 32;
  static uint64_t ThreadKey() {
    static std::atomic<uint32_t> next{1};
    thread_local const uint32_t key = next.fetch_add(1, std::memory_order_relaxed);
    return key;
  }
  void SlowEnter() override { state_.fetch_add(kSlowOne, std::memory_order_seq_cst); }
  void SlowLeave() override { state_.fetch_sub(kSlowOne, std::memory_order_seq_cst); }
  bool TryFastAcquire() override {
    const uint64_t me = ThreadKey();
    uint64_t s = state_.load(std::memory_order_relaxed);
    if ((s & kOwnerMask) == me) {
      if (count_.load(std::memory_order_relaxed) == 0) return false;
      count_.fetch_add(1, std::memory_order_relaxed);
      return true;
    }
    if (s == 0 && state_.compare_exchange_strong(s, me, std::memory_order_acquire, std::memory_order_relaxed)) {
      count_.store(1, std::memory_order_relaxed);
      return true;
    }
    return false;
  }
  bool signaled() const override {
    const uint64_t owner = state_.load(std::memory_order_seq_cst) & kOwnerMask;
    return owner == 0 || owner == ThreadKey();
  }
  void post_execution() override {
    const uint64_t me = ThreadKey();
    uint64_t s = state_.load(std::memory_order_relaxed);
    if ((s & kOwnerMask) == me) {
      count_.fetch_add(1, std::memory_order_relaxed);
      return;
    }
    if ((s & kOwnerMask) != 0) {
      std::fprintf(stderr, "FAIL: post_execution found the mutant owned by key %u\n", unsigned(s & kOwnerMask));
      std::exit(2);
    }
    while (!state_.compare_exchange_weak(s, (s & ~kOwnerMask) | me, std::memory_order_acq_rel,
                                         std::memory_order_relaxed)) {
    }
    count_.store(1, std::memory_order_relaxed);
  }
  std::atomic<uint64_t> state_{0};
  std::atomic<uint32_t> count_{0};
};

// ---- a manual-reset event with PulseEvent (PosixCondition<Event>, condition-variable flavour) ----
class Event : public Cond {
 public:
  bool Signal() override {
    {
      std::lock_guard<std::mutex> lock(mutex_);
      signal_ = true;
    }
    cond_.notify_all();
    NotifyMulti();
    return true;
  }
  void Pulse() {
    {
      std::lock_guard<std::mutex> lock(mutex_);
      if (cond_waiters_ + armed_ == 0) return;
      ++pulse_gen_;
    }
    cond_.notify_all();
    NotifyMulti();
  }

 private:
  bool signaled() const override { return signal_; }
  void post_execution() override {}
  bool signal_ = false;
};

struct Mutant {
  OldMutant o;
  NewMutant n;
  Cond& c() { return g_old ? static_cast<Cond&>(o) : static_cast<Cond&>(n); }
  bool Release() { return g_old ? o.Release() : n.Release(); }
};

WaitResult SignalAndWait(Mutant& sig, Cond& wait_on, ms timeout) {
  Cond::Waiter w;
  const uint64_t gen = wait_on.Arm(&w);
  if (!sig.Release()) {
    std::fprintf(stderr, "FAIL: SignalAndWait released a mutant it did not own\n");
    std::exit(2);
  }
  WaitResult r = wait_on.Wait(timeout, &gen, &w);
  wait_on.Disarm(&w, r == WaitResult::kSuccess);
  return r;
}

[[noreturn]] void Fail(const char* what) {
  std::fprintf(stderr, "FAIL: %s\n", what);
  std::exit(1);
}

void Solo(int seconds) {
  Mutant m;
  const auto end = Clock::now() + std::chrono::seconds(seconds);
  uint64_t pairs = 0;
  const auto t0 = Clock::now();
  while (Clock::now() < end) {
    for (int i = 0; i < 4096; ++i) {
      m.c().Wait(ms::max());
      if ((i & 3) == 0) {
        m.c().Wait(ms::max());
        m.Release();
      }
      m.Release();
    }
    pairs += 4096;
  }
  const double ns = std::chrono::duration<double, std::nano>(Clock::now() - t0).count() / double(pairs);
  std::printf("solo: %llu lock/unlock pairs, %.1f ns a pair\n", static_cast<unsigned long long>(pairs), ns);
}

void Lock(int seconds) {
  Mutant m;
  Event idle;  // never signalled: WaitAny on {mutant, idle}
  constexpr int kThreads = 6;
  std::atomic<bool> stop{false};
  std::atomic<int> inside{0};
  uint64_t counter = 0;  // guarded by the mutant
  std::atomic<uint64_t> acquired{0}, timeouts{0}, progress{0};
  std::vector<std::thread> threads;
  for (int t = 0; t < kThreads; ++t) {
    threads.emplace_back([&, t] {
      std::mt19937 rng(1234 + t);
      uint64_t local = 0;
      while (!stop.load(std::memory_order_relaxed)) {
        const int kind = int(rng() % 8);
        WaitResult r;
        if (kind == 0) {
          r = m.c().Wait(ms(0));
        } else if (kind == 1) {
          r = m.c().Wait(ms(1));
        } else if (kind == 2) {
          auto res = Cond::WaitAny({&m.c(), &idle}, ms(5));
          r = res.first;
          if (r == WaitResult::kSuccess && res.second != 0) Fail("WaitAny picked the idle event");
        } else {
          r = m.c().Wait(ms::max());
        }
        if (r != WaitResult::kSuccess) {
          timeouts.fetch_add(1, std::memory_order_relaxed);
          continue;
        }
        if (inside.fetch_add(1, std::memory_order_acq_rel) != 0) Fail("two threads inside the mutant");
        const int depth = int(rng() % 3);
        for (int d = 0; d < depth; ++d) {
          if (m.c().Wait(ms(0)) != WaitResult::kSuccess) Fail("recursive acquire failed");
        }
        counter++;
        if ((rng() & 15) == 0) std::this_thread::yield();
        for (int d = 0; d < depth; ++d) {
          if (!m.Release()) Fail("recursive release failed");
        }
        if (inside.fetch_sub(1, std::memory_order_acq_rel) != 1) Fail("inside count broken");
        if (!m.Release()) Fail("release failed");
        // A release by a thread that no longer owns it must fail and change nothing. Not on "old": its Release reads
        // owner_ / count_ without the mutex, so it can see its own stale owner_ next to the new owner's count and
        // release the new owner's hold (a data race of the previous code; the game never releases twice).
        if (!g_old && m.Release()) Fail("release of a mutant not owned succeeded");
        ++local;
        progress.fetch_add(1, std::memory_order_relaxed);
      }
      acquired.fetch_add(local);
    });
  }
  const auto t0 = Clock::now();
  uint64_t last = 0;
  auto last_change = Clock::now();
  while (Clock::now() - t0 < std::chrono::seconds(seconds)) {
    std::this_thread::sleep_for(ms(50));
    const uint64_t p = progress.load();
    if (p != last) {
      last = p;
      last_change = Clock::now();
    } else if (Clock::now() - last_change > std::chrono::seconds(2)) {
      std::fprintf(stderr, "STALL: lock, no progress for 2 s at %llu\n", static_cast<unsigned long long>(p));
      std::fflush(stderr);
      std::_Exit(3);
    }
  }
  stop = true;
  for (auto& th : threads) th.join();
  const double s = std::chrono::duration<double>(Clock::now() - t0).count();
  if (counter != acquired.load()) Fail("counter != acquisitions (mutual exclusion broken)");
  std::printf("lock: %llu acquisitions (%.0f/s), %llu timeouts, counter ok\n",
              static_cast<unsigned long long>(counter), double(counter) / s,
              static_cast<unsigned long long>(timeouts.load()));
}

void Condvar(int seconds) {
  Mutant m;
  Event cv;
  constexpr int kProducers = 3, kConsumers = 3;
  std::atomic<bool> stop{false};
  uint64_t queue = 0;   // guarded by the mutant
  bool closing = false;  // guarded by the mutant
  std::atomic<uint64_t> pushed{0}, popped{0}, progress{0}, sigwaits{0};
  std::atomic<int> exited{0};
  std::vector<std::thread> threads;
  for (int p = 0; p < kProducers; ++p) {
    threads.emplace_back([&, p] {
      std::mt19937 rng(77 + p);
      while (!stop.load(std::memory_order_relaxed)) {
        m.c().Wait(ms::max());
        ++queue;
        m.Release();
        pushed.fetch_add(1, std::memory_order_relaxed);
        cv.Pulse();
        progress.fetch_add(1, std::memory_order_relaxed);
        if ((rng() & 7) == 0) std::this_thread::yield();
      }
    });
  }
  for (int c = 0; c < kConsumers; ++c) {
    threads.emplace_back([&] {
      for (;;) {
        m.c().Wait(ms::max());
        while (queue == 0 && !closing) {
          SignalAndWait(m, cv, ms::max());
          sigwaits.fetch_add(1, std::memory_order_relaxed);
          m.c().Wait(ms::max());
        }
        if (queue == 0 && closing) {
          m.Release();
          exited.fetch_add(1);
          return;
        }
        --queue;
        m.Release();
        popped.fetch_add(1, std::memory_order_relaxed);
        progress.fetch_add(1, std::memory_order_relaxed);
      }
    });
  }
  const auto t0 = Clock::now();
  uint64_t last = 0;
  auto last_change = Clock::now();
  auto check = [&] {
    const uint64_t pr = progress.load();
    if (pr != last) {
      last = pr;
      last_change = Clock::now();
    } else if (Clock::now() - last_change > std::chrono::seconds(2)) {
      std::fprintf(stderr, "STALL: condvar, no progress for 2 s (pushed %llu popped %llu)\n",
                   static_cast<unsigned long long>(pushed.load()), static_cast<unsigned long long>(popped.load()));
      std::fflush(stderr);
      std::_Exit(3);
    }
  };
  while (Clock::now() - t0 < std::chrono::seconds(seconds)) {
    std::this_thread::sleep_for(ms(50));
    check();
  }
  stop = true;
  for (int p = 0; p < kProducers; ++p) threads[p].join();
  m.c().Wait(ms::max());
  closing = true;
  m.Release();
  // A consumer that relocks after this sees `closing` (set under the mutant); one parked in SignalAndWait needs a
  // pulse: keep pulsing until every consumer has left.
  const auto close_start = Clock::now();
  while (exited.load() != kConsumers) {
    cv.Pulse();
    std::this_thread::sleep_for(ms(1));
    if (Clock::now() - close_start > std::chrono::seconds(2)) {
      std::fprintf(stderr, "STALL: condvar, consumers did not leave (%d of %d)\n", exited.load(), kConsumers);
      std::_Exit(3);
    }
  }
  for (int c = kProducers; c < kProducers + kConsumers; ++c) threads[c].join();
  const double s = std::chrono::duration<double>(Clock::now() - t0).count();
  if (pushed.load() != popped.load() || queue != 0) Fail("items lost or duplicated");
  std::printf("condvar: %llu items (%.0f/s), %llu SignalAndWait, all popped once\n",
              static_cast<unsigned long long>(pushed.load()), double(pushed.load()) / s,
              static_cast<unsigned long long>(sigwaits.load()));
}

}  // namespace

int main(int argc, char** argv) {
  if (argc < 2) {
    std::printf("usage: %s <solo|lock|condvar|all> [seconds=5] [old|new|fastonly|lockfreeonly]\n", argv[0]);
    return 1;
  }
  const char* mode = argv[1];
  const int seconds = argc > 2 ? std::max(1, std::atoi(argv[2])) : 5;
  const char* variant = argc > 3 ? argv[3] : "new";
  if (!std::strcmp(variant, "old")) {
    g_old = true;
  } else if (!std::strcmp(variant, "fastonly")) {
    g_lockfree = false;
  } else if (!std::strcmp(variant, "lockfreeonly")) {
    g_fast = false;
  }
  std::printf("[%s]\n", variant);
  const bool all = !std::strcmp(mode, "all");
  if (all || !std::strcmp(mode, "solo")) Solo(seconds);
  if (all || !std::strcmp(mode, "lock")) Lock(seconds);
  if (all || !std::strcmp(mode, "condvar")) Condvar(seconds);
  std::printf("fast acquires %llu, slow waits %llu, releases with no slow waiter (no mutex, no notify) %llu, "
              "notifies %llu\n",
              static_cast<unsigned long long>(g_fast_hits.load()), static_cast<unsigned long long>(g_slow_waits.load()),
              static_cast<unsigned long long>(g_idle_releases.load()),
              static_cast<unsigned long long>(g_notifies.load()));
  return 0;
}
