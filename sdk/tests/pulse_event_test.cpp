// Stress test of the event hand-off, PulseEvent and SignalObjectAndWait as sdk/src/core/threading_posix.cpp does them on
// Switch (PosixConditionBase::Arm/Disarm/Wait/GrantOrSet, PosixCondition<Event>::Signal/Pulse). The class below repeats
// that file's logic so it runs on a PC, where threading_posix.cpp is not built. Two patterns from Tekken 6, each a
// strict handshake: a lost wake-up leaves a thread blocked for ever, which the watchdog reports.
//  - barrier: workers take a lock, count themselves, SignalObjectAndWait(lock, manual-reset event); the controller
//    sees every worker counted and PulseEvent(event)s (sub_822E1B28 / sub_822E1AA0).
//  - pingpong: one condition variable (mutex + auto-reset event + value) shared by the main thread, which sets 1 and
//    waits for 0, and a worker, which waits for 1 and sets 0 (sub_82519918 / sub_82519928 / sub_82519EC0).
//   clang++ -O2 -std=c++20 sdk/tests/pulse_event_test.cpp -o out/tools/pulse_event_test.exe
//   pulse_event_test barrier|pingpong [seconds=5] [old]   ("old": the former code; expected to deadlock)
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
#include <thread>
#include <vector>

namespace {

bool g_old = false;

class Event {
 public:
  struct Waiter {
    bool granted = false;
  };
  Event(bool manual_reset, bool initial) : signal_(initial), manual_reset_(manual_reset) {}

  uint64_t Arm(Waiter* w) {
    std::lock_guard<std::mutex> lock(mutex_);
    ++armed_;
    if (!manual_reset_) queue_.push_back(w);
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
    if (pass_on) cond_.notify_all();
  }
  void Wait(const uint64_t* armed_gen = nullptr, Waiter* armed = nullptr) {
    std::unique_lock<std::mutex> lock(mutex_);
    Waiter local;
    Waiter* me = armed ? armed : &local;
    const uint64_t gen = armed_gen ? *armed_gen : pulse_gen_;
    auto predicate = [&] { return me->granted || signal_ || pulse_gen_ != gen; };
    if (!predicate()) {
      if (!g_old && !manual_reset_ && !armed) queue_.push_back(me);
      ++waiters_;
      cond_.wait(lock, predicate);
      --waiters_;
    }
    if (me->granted) return;
    if (signal_) {
      if (armed) Dequeue(me);
      if (!manual_reset_) signal_ = false;
    }
  }
  void Set() {
    {
      std::lock_guard<std::mutex> lock(mutex_);
      if (g_old || manual_reset_) signal_ = true; else GrantOrSet();
    }
    cond_.notify_all();
  }
  void Reset() {
    std::lock_guard<std::mutex> lock(mutex_);
    signal_ = false;
  }
  void Pulse() {
    if (g_old) {
      Set();
      // A real 10 us, as svcSleepThread on Horizon (a Windows sleep_for(10us) lasts >= 1 ms and hides the race).
      const auto until = std::chrono::steady_clock::now() + std::chrono::microseconds(10);
      while (std::chrono::steady_clock::now() < until) std::this_thread::yield();
      Reset();
      return;
    }
    {
      std::lock_guard<std::mutex> lock(mutex_);
      if (waiters_ + armed_ == 0) return;
      if (manual_reset_) ++pulse_gen_; else GrantOrSet();
    }
    cond_.notify_all();
  }

 private:
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
  bool signal_;
  const bool manual_reset_;
  uint32_t waiters_ = 0, armed_ = 0;
  uint64_t pulse_gen_ = 0;
  std::deque<Waiter*> queue_;
};

void SignalAndWait(Event& to_signal, Event& wait_on) {
  if (g_old) {  // the former order: signal, then start waiting
    to_signal.Set();
    wait_on.Wait();
    return;
  }
  Event::Waiter w;
  const uint64_t gen = wait_on.Arm(&w);
  to_signal.Set();
  wait_on.Wait(&gen, &w);
  wait_on.Disarm(&w, true);
}

std::atomic<bool> g_stop{false};
std::atomic<uint64_t> g_rounds{0};

std::vector<std::thread> Barrier() {
  constexpr int kWorkers = 7;  // Main XThread + 6 workers
  static Event lock(false, true), cv(true, false);
  static int arrived = 0;  // guarded by `lock`
  std::vector<std::thread> threads;
  for (int i = 0; i < kWorkers; ++i) {
    threads.emplace_back([] {
      while (!g_stop.load()) {
        lock.Wait();
        ++arrived;
        SignalAndWait(lock, cv);
      }
    });
  }
  threads.emplace_back([] {
    while (!g_stop.load()) {
      lock.Wait();
      if (arrived == kWorkers) {
        arrived = 0;
        cv.Pulse();
        g_rounds.fetch_add(1);
      }
      lock.Set();
      std::this_thread::yield();
    }
    for (int i = 0; i < 1000; ++i) cv.Pulse(), std::this_thread::sleep_for(std::chrono::milliseconds(1));
  });
  return threads;
}

// sub_82528C10 (set value, SetEvent) and sub_82528C68 (wait until value == target).
struct CondValue {
  Event lock{false, true}, ev{false, false};
  int value = 0;
  void SetValue(int v) {
    lock.Wait();
    value = v;
    lock.Set();
    ev.Set();
  }
  void WaitValue(int target) {
    lock.Wait();
    while (value != target && !g_stop.load()) {
      SignalAndWait(lock, ev);
      lock.Wait();
    }
    lock.Set();
  }
};

std::vector<std::thread> PingPong() {
  static CondValue cv;
  std::vector<std::thread> threads;
  threads.emplace_back([] {  // worker
    while (!g_stop.load()) {
      cv.WaitValue(1);
      cv.SetValue(0);
    }
  });
  threads.emplace_back([] {  // main
    while (!g_stop.load()) {
      cv.SetValue(1);
      cv.WaitValue(0);
      g_rounds.fetch_add(1);
    }
    for (int i = 0; i < 1000; ++i) cv.ev.Set(), std::this_thread::sleep_for(std::chrono::milliseconds(1));
  });
  return threads;
}

}  // namespace

int main(int argc, char** argv) {
  const bool pingpong = argc > 1 && std::strcmp(argv[1], "pingpong") == 0;
  const int seconds = argc > 2 ? std::atoi(argv[2]) : 5;
  g_old = argc > 3 && std::strcmp(argv[3], "old") == 0;
  auto threads = pingpong ? PingPong() : Barrier();

  uint64_t last = 0;
  for (int s = 0; s < seconds; ++s) {
    std::this_thread::sleep_for(std::chrono::seconds(1));
    const uint64_t now = g_rounds.load();
    if (now == last) {
      std::printf("FAIL %s: no round for 1 s after %llu rounds (lost wake-up)\n", pingpong ? "pingpong" : "barrier",
                  (unsigned long long)now);
      std::fflush(stdout);
      std::_Exit(1);
    }
    last = now;
  }
  g_stop.store(true);
  for (auto& t : threads) t.join();
  std::printf("OK %s: %llu rounds, no lost wake-up\n", pingpong ? "pingpong" : "barrier",
              (unsigned long long)g_rounds.load());
  return 0;
}
